#include "../shell/HistorySearch.h"
#include "../shell/HotkeyConfig.h"
#include "../shell/LineEditing.h"
#include "../shell/SharedHistory.h"
#include "../gui/X11Clipboard.h"
#include "../gui/MouseSelection.h"
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
    size_t cursor = 0;
    std::vector<std::string> history;
    std::string historyDraft;
    size_t historyPosition = 0;
    bool browsingHistory = false;
    bool searchingHistory = false;
    std::string historySavedInput;
    size_t historySavedCursor = 0;
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
    XSelectInput(display, window, ExposureMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask);
    GC gc = XCreateGC(display, window, 0, nullptr);
    if (!gc) { XDestroyWindow(display, window); XCloseDisplay(display); return 1; }
    X11Clipboard clipboard(display, window);
    XMapWindow(display, window);

    std::vector<ProcessTab> tabs;
    auto newTab = [&]() -> bool {
        if (available.empty()) return false;
        ProcessTab tab;
        tab.history = SharedHistory::load();
        tab.child = std::move(available.back());
        available.pop_back();
        tabs.push_back(std::move(tab));
        return true;
    };
    newTab();
    size_t active = 0;
    bool running = true, dirty = true;
    bool inputDirty = false;
    MouseSelection selection;
    XFontStruct* selectionFont = XQueryFont(display, XGContextFromGC(gc));
    selection.setFont(selectionFont);
    size_t selectionTab = 0;
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
                                           (tab.multilinePending.empty() ? "user@terminator> " : "> ") + (tab.searchingHistory ? std::string("Enter search term: ") : std::string()) +
            LineEditing::display(tab.input, tab.cursor);
        XDrawString(display, window, gc, 20, y, prompt.c_str(),
                    static_cast<int>(prompt.size()));
        if (selectionTab == active) selection.draw(display, window, gc);
        XFlush(display);
        dirty = false;
    };

    auto drawInput = [&] {
        const ProcessTab& tab = tabs[active];
        const int visible = 20;
        int shown = std::min(static_cast<int>(tab.lines.size()), visible - 1);
        int y = 65 + shown * 22;
        XClearArea(display, window, 20, y - 17, 850, 22, False);
        std::string prompt = tab.busy ? "[command running...]" :
            (tab.multilinePending.empty() ? "user@terminator> " : "> ") +
            (tab.searchingHistory ? std::string("Enter search term: ") : std::string()) +
            LineEditing::display(tab.input, tab.cursor);
        XDrawString(display, window, gc, 20, y, prompt.c_str(),
                    static_cast<int>(prompt.size()));
        XFlush(display);
    };
    while (running) {
        while (XPending(display)) {
            XEvent event;
            XNextEvent(display, &event);
            std::string pasted;
            if (clipboard.handle(event, pasted)) {
                if (!pasted.empty() && !tabs[active].busy) {
                    for (unsigned char c : pasted)
                        if (c >= 32 && c != 127)
                            LineEditing::insert(tabs[active].input, tabs[active].cursor, c);
                    drawInput();
                }
                continue;
            }
            if (event.type == Expose) { dirty = true; continue; }
            if (event.type == ButtonPress && event.xbutton.button == Button1 && event.xbutton.y >= 48) {
                const ProcessTab& tab = tabs[active];
                std::vector<std::string> visible;
                int start = std::max(0, static_cast<int>(tab.lines.size()) - 19);
                for (int i=start; i<static_cast<int>(tab.lines.size()); ++i)
                    visible.push_back(tab.lines[i]);
                visible.push_back(tab.busy ? "[command running...]" :
                    (tab.multilinePending.empty() ? "user@terminator> " : "> ") +
                    LineEditing::display(tab.input, tab.cursor));
                selectionTab = active;
                selection.begin(event.xbutton.x, event.xbutton.y, std::move(visible));
                dirty = true;
                continue;
            }
            if (event.type == MotionNotify && (event.xmotion.state & Button1Mask)) {
                if (selectionTab == active) { selection.move(event.xmotion.x, event.xmotion.y); dirty = true; }
                continue;
            }
            if (event.type == ButtonRelease && event.xbutton.button == Button1) {
                if (selectionTab == active) { selection.finish(event.xbutton.x, event.xbutton.y); dirty = true; }
                continue;
            }
            if (event.type != KeyPress) continue;
            KeySym key;
            char buffer[32];
            int count = XLookupString(&event.xkey, buffer, sizeof(buffer), &key, nullptr);
            HotkeyConfig::translate(key, event.xkey.state);
            ProcessTab& searchTab = tabs[active];
            if ((event.xkey.state & ControlMask) && (key == XK_r || key == XK_R) &&
                !searchTab.busy && !searchTab.searchingHistory) {
                searchTab.searchingHistory = true;
                searchTab.historySavedInput = searchTab.input;
                searchTab.historySavedCursor = searchTab.cursor;
                searchTab.input.clear(); searchTab.cursor = 0;
                dirty = true;
                continue;
            }
            if (searchTab.searchingHistory) {
                if (key == XK_Return || key == XK_KP_Enter) {
                    std::string term = searchTab.input;
                    searchTab.searchingHistory = false;
                    searchTab.input = searchTab.historySavedInput;
                    searchTab.cursor = searchTab.historySavedCursor;
                    searchTab.lines.push_back("Enter search term: " + term);
                    auto matches = HistorySearch::find(term);
                    if (matches.empty()) searchTab.lines.push_back("No match for search term in history");
                    else for (const auto& command : matches) searchTab.lines.push_back(command);
                    dirty = true;
                } else if (key == XK_Escape) {
                    searchTab.searchingHistory = false;
                    searchTab.input = searchTab.historySavedInput;
                    searchTab.cursor = searchTab.historySavedCursor;
                    dirty = true;
                } else if (LineEditing::apply(searchTab.input, searchTab.cursor, key,
                                              (event.xkey.state & ControlMask) != 0)) {
                    inputDirty = true;
                } else if (!(event.xkey.state & ControlMask) && count > 0) {
                    for (int i = 0; i < count; ++i)
                        if (buffer[i] >= 32 && buffer[i] <= 126)
                            LineEditing::insert(searchTab.input, searchTab.cursor, buffer[i]);
                    inputDirty = true;
                }
                continue;
            }
            if (key == XK_Escape) {
                bool anyBusy = false;
                for (const auto& tab : tabs) anyBusy |= tab.busy;
                if (!anyBusy) running = false;
                else tabs[active].lines.push_back("Wait for running commands before exit.");
                dirty = true;
                continue;
            }
            if ((event.xkey.state & ControlMask) && (event.xkey.state & ShiftMask)) {
                if (key == XK_c || key == XK_C) {
                    clipboard.copy(selectionTab == active && !selection.selectedText().empty()
                                   ? selection.selectedText() : tabs[active].input);
                    continue;
                }
                if (key == XK_v || key == XK_V) {
                    if (!tabs[active].busy) clipboard.requestPaste();
                    continue;
                }
            }
            if ((event.xkey.state & ControlMask) && (key == XK_l || key == XK_L)) {
                tabs[active].lines.clear();
                dirty = true;
                continue;
            }
            if ((event.xkey.state & ControlMask) && (key == XK_d || key == XK_D)) {
                if (!tabs[active].busy && tabs[active].input.empty() &&
                    tabs[active].multilinePending.empty()) {
                    if (tabs.size() > 1) {
                        available.push_back(std::move(tabs[active].child));
                        tabs.erase(tabs.begin() + static_cast<long>(active));
                        if (active >= tabs.size()) active = tabs.size() - 1;
                    } else running = false;
                    dirty = true;
                }
                continue;
            }
            if (event.xkey.state & ControlMask) {
                if (key == XK_t || key == XK_T) {
                    if (newTab()) active = tabs.size() - 1;
                    else tabs[active].lines.push_back("Maximum 4 process tabs in P6.");
                } else if (key == XK_Tab) {
                    active = (active + 1) % tabs.size();
                } else if ((key == XK_w || key == XK_W) &&
                           (event.xkey.state & ShiftMask)) {
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
                } else if (LineEditing::apply(tabs[active].input, tabs[active].cursor,
                                              key, true)) {
                    // Only the prompt changes; avoid clearing command output.
                    inputDirty = true;
                } else if (key == XK_c || key == XK_C) {
                    if (tabs[active].busy) {
                        if (!tabs[active].child->interrupt())
                            tabs[active].lines.push_back("Unable to interrupt process child.");
                        else tabs[active].lines.push_back("^C");
                    } else tabs[active].lines.push_back("^C");
                    if (!tabs[active].busy) { tabs[active].input.clear(); tabs[active].cursor = 0;
                        tabs[active].multilinePending.clear(); }
                } else if (key == XK_z || key == XK_Z) {
                    if (tabs[active].busy) {
                        if (!tabs[active].child->suspend())
                            tabs[active].lines.push_back("Unable to suspend process child.");
                        else tabs[active].lines.push_back("^Z");
                    }
                }
                if (!inputDirty) dirty = true;
                continue;
            }
            ProcessTab& tab = tabs[active];
            if (tab.busy) continue;
            bool editedInput = true;
            if ((key == XK_Up || key == XK_Down) && tab.multilinePending.empty()) {
                if (key == XK_Up) tab.history = SharedHistory::load();
                if (key == XK_Up && !tab.history.empty()) {
                    if (!tab.browsingHistory) {
                        tab.historyDraft = tab.input;
                        tab.historyPosition = tab.history.size();
                        tab.browsingHistory = true;
                    }
                    if (tab.historyPosition > 0) --tab.historyPosition;
                    tab.input = tab.history[tab.historyPosition];
                    tab.cursor = tab.input.size();
                } else if (key == XK_Down && tab.browsingHistory) {
                    if (tab.historyPosition + 1 < tab.history.size()) {
                        ++tab.historyPosition;
                        tab.input = tab.history[tab.historyPosition];
                    tab.cursor = tab.input.size();
                    } else {
                        tab.input = tab.historyDraft;
                        tab.cursor = tab.input.size();
                        tab.browsingHistory = false;
                    }
                }
            } else if (LineEditing::apply(tab.input, tab.cursor, key, false)) {
                // Cursor motion, Backspace and Delete.
            } else if (key == XK_Return || key == XK_KP_Enter) {
                editedInput = false;
                std::string command = tab.multilinePending;
                tab.browsingHistory = false;
                tab.historyDraft.clear();
                MultilineInput::appendLine(command, tab.input);
                tab.input.clear();
                tab.cursor = 0;
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
                    SharedHistory::append(command);
                    tab.history = SharedHistory::load();
                    tab.lines.push_back("user@terminator> " + command);
                    if (command == "history") {
                        auto recent = SharedHistory::recent();
                        for (size_t i = 0; i < recent.size(); ++i)
                            tab.lines.push_back(std::to_string(i + 1) + "  " + recent[i]);
                    } else if (!tab.child->submit(command))
                        tab.lines.push_back("Failed to send command.");
                    else tab.busy = true;
                    tab.input.clear();
                }
            } else {
                for (int i = 0; i < count; ++i)
                    if (buffer[i] >= 32 && buffer[i] <= 126)
                        LineEditing::insert(tab.input, tab.cursor, buffer[i]);
            }
            if (editedInput) inputDirty = true;
            else dirty = true;
        }
        for (auto& tab : tabs) {
            for (auto& response : tab.child->takeReplies()) {
                if (response.second == std::string("\x01DONE", 5)) {
                    tab.busy = false;
                } else {
                    addOutput(tab, response.second);
                    // multiWatch replies are streamed; the final DONE frame clears busy.
                    if (response.first.rfind("multiWatch", 0) != 0 ||
                        response.second.rfind("multiWatch:", 0) == 0)
                        tab.busy = false;
                }
                dirty = true;
            }
            if (tab.child->hasFailed()) {
                tab.lines.push_back("Child IPC failed.");
                running = false;
                dirty = true;
            }
        }
        if (dirty) { draw(); inputDirty = false; }
        else if (inputDirty) { drawInput(); inputDirty = false; }
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
