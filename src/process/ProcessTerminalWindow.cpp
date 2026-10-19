#include "AsyncProcessChannel.h"
#include "../shell/MultilineInput.h"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <poll.h>
#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {
constexpr int MAX_TABS = 4;
struct ProcessTab {
    std::unique_ptr<AsyncProcessChannel> child;
    std::vector<std::string> lines{"Welcome to Terminator process mode"};
    std::string input;
    std::vector<std::string> history;
    std::string historyDraft;
    size_t historyPosition = 0;
    bool browsingHistory = false;
    std::string multilinePending;
    bool busy = false;
};
void addOutput(ProcessTab& tab, const std::string& output) {
    size_t pos = 0;
    while (pos < output.size()) {
        size_t end = output.find('\n', pos);
        if (end == std::string::npos) {
            tab.lines.push_back(output.substr(pos));
            break;
        }
        tab.lines.push_back(output.substr(pos, end - pos));
        pos = end + 1;
    }
}
}

int runProcessWindow() {
    // Pre-fork a bounded pool BEFORE starting any threads or opening X11.
    // A new tab claims one child; each child owns its own ShellSession.
    std::vector<std::unique_ptr<AsyncProcessChannel>> available;
    for (int i = 0; i < MAX_TABS; ++i) {
        auto child = std::make_unique<AsyncProcessChannel>();
        if (!child->startProcess()) {
            std::fprintf(stderr, "Cannot fork process-mode child %d\n", i + 1);
            return 1;
        }
        available.push_back(std::move(child));
    }
    for (auto& child : available) {
        if (!child->startReader()) {
            std::fprintf(stderr, "Cannot start child IPC reader\n");
            return 1;
        }
    }
    Display* display = XOpenDisplay(nullptr);
    if (!display) { std::fprintf(stderr, "Cannot open X display\n"); return 1; }
    int screen = DefaultScreen(display);
    Window window = XCreateSimpleWindow(display, RootWindow(display, screen),
                                        100, 100, 900, 550, 1,
                                        BlackPixel(display, screen),
                                        WhitePixel(display, screen));
    if (!window) { XCloseDisplay(display); return 1; }
    XSelectInput(display, window, ExposureMask | KeyPressMask);
    GC gc = XCreateGC(display, window, 0, nullptr);
    if (!gc) { XDestroyWindow(display, window); XCloseDisplay(display); return 1; }
    XMapWindow(display, window);

    std::vector<ProcessTab> tabs;
    auto newTab = [&]() -> bool {
        if (available.empty()) return false;
        ProcessTab tab;
        tab.child = std::move(available.back());
        available.pop_back();
        tabs.push_back(std::move(tab));
        return true;
    };
    newTab();
    size_t active = 0;
    bool running = true, dirty = true;
    auto draw = [&] {
        XClearWindow(display, window);
        int x = 20;
        for (size_t i = 0; i < tabs.size(); ++i) {
            std::string label = (i == active ? "[ " : "  ") +
                std::string("Tab ") + std::to_string(i + 1) +
                " PID:" + std::to_string(tabs[i].child->pid()) +
                (i == active ? " ]" : "  ");
            XDrawString(display, window, gc, x, 25, label.c_str(),
                        static_cast<int>(label.size()));
            x += 215;
        }
        XDrawLine(display, window, gc, 10, 38, 880, 38);
        const ProcessTab& tab = tabs[active];
        const int visible = 20;
        int start = std::max(0, static_cast<int>(tab.lines.size()) - (visible - 1));
        int y = 65;
        for (int i = start; i < static_cast<int>(tab.lines.size()); ++i, y += 22) {
            XDrawString(display, window, gc, 20, y, tab.lines[i].c_str(),
                        static_cast<int>(tab.lines[i].size()));
        }
        std::string prompt = tab.busy ? "[command running...]" :
                                           (tab.multilinePending.empty() ? "user@terminator> " : "> ") + tab.input + "_";
        XDrawString(display, window, gc, 20, y, prompt.c_str(),
                    static_cast<int>(prompt.size()));
        XFlush(display);
        dirty = false;
    };

    while (running) {
        while (XPending(display)) {
            XEvent event;
            XNextEvent(display, &event);
            if (event.type == Expose) { dirty = true; continue; }
            if (event.type != KeyPress) continue;
            KeySym key;
            char buffer[32];
            int count = XLookupString(&event.xkey, buffer, sizeof(buffer), &key, nullptr);
            if (key == XK_Escape) {
                bool anyBusy = false;
                for (const auto& tab : tabs) anyBusy |= tab.busy;
                if (!anyBusy) running = false;
                else tabs[active].lines.push_back("Wait for running commands before exit.");
                dirty = true;
                continue;
            }
            if (event.xkey.state & ControlMask) {
                if (key == XK_t || key == XK_T) {
                    if (newTab()) active = tabs.size() - 1;
                    else tabs[active].lines.push_back("Maximum 4 process tabs in P6.");
                } else if (key == XK_Tab) {
                    active = (active + 1) % tabs.size();
                } else if (key == XK_w || key == XK_W) {
                    if (tabs[active].busy) {
                        tabs[active].lines.push_back("Wait for command to finish before closing.");
                    } else if (tabs.size() > 1) {
                        // Keep the child alive for reuse; no fork after threads exist.
                        available.push_back(std::move(tabs[active].child));
                        tabs.erase(tabs.begin() + static_cast<long>(active));
                        if (active >= tabs.size()) active = tabs.size() - 1;
                    } else {
                        running = false;
                    }
                } else if (key == XK_c || key == XK_C) {
                    tabs[active].lines.push_back(tabs[active].busy ?
                        "P6: Ctrl+C pending signal IPC." : "^C");
                    if (!tabs[active].busy) { tabs[active].input.clear(); tabs[active].multilinePending.clear(); }
                } else if (key == XK_z || key == XK_Z) {
                    tabs[active].lines.push_back("P6: Ctrl+Z pending job-control IPC.");
                }
                dirty = true;
                continue;
            }
            ProcessTab& tab = tabs[active];
            if (tab.busy) continue;
            if ((key == XK_Up || key == XK_Down) && tab.multilinePending.empty()) {
                if (key == XK_Up && !tab.history.empty()) {
                    if (!tab.browsingHistory) {
                        tab.historyDraft = tab.input;
                        tab.historyPosition = tab.history.size();
                        tab.browsingHistory = true;
                    }
                    if (tab.historyPosition > 0) --tab.historyPosition;
                    tab.input = tab.history[tab.historyPosition];
                } else if (key == XK_Down && tab.browsingHistory) {
                    if (tab.historyPosition + 1 < tab.history.size()) {
                        ++tab.historyPosition;
                        tab.input = tab.history[tab.historyPosition];
                    } else {
                        tab.input = tab.historyDraft;
                        tab.browsingHistory = false;
                    }
                }
            } else if (key == XK_BackSpace) {
                if (!tab.input.empty()) tab.input.pop_back();
            } else if (key == XK_Return || key == XK_KP_Enter) {
                std::string command = tab.multilinePending;
                tab.browsingHistory = false;
                tab.historyDraft.clear();
                MultilineInput::appendLine(command, tab.input);
                tab.input.clear();
                if (MultilineInput::needsMore(command)) {
                    tab.multilinePending = std::move(command);
                    dirty = true;
                    continue;
                }
                tab.multilinePending.clear();
                if (command == "exit") {
                    if (tabs.size() == 1) running = false;
                    else {
                        available.push_back(std::move(tab.child));
                        tabs.erase(tabs.begin() + static_cast<long>(active));
                        if (active >= tabs.size()) active = tabs.size() - 1;
                    }
                } else if (!command.empty()) {
                    tab.history.push_back(command);
                    tab.lines.push_back("user@terminator> " + command);
                    if (!tab.child->submit(command))
                        tab.lines.push_back("Failed to send command.");
                    else tab.busy = true;
                    tab.input.clear();
                }
            } else {
                for (int i = 0; i < count; ++i)
                    if (buffer[i] >= 32 && buffer[i] <= 126) tab.input += buffer[i];
            }
            dirty = true;
        }
        for (auto& tab : tabs) {
            for (auto& response : tab.child->takeReplies()) {
                addOutput(tab, response.second);
                tab.busy = false;
                dirty = true;
            }
            if (tab.child->hasFailed()) {
                tab.lines.push_back("Child IPC failed.");
                running = false;
                dirty = true;
            }
        }
        if (dirty) draw();
        pollfd xfd{};
        xfd.fd = ConnectionNumber(display);
        xfd.events = POLLIN;
        if (poll(&xfd, 1, 20) < 0) break;
    }
    XFreeGC(display, gc);
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    return 0;
}
