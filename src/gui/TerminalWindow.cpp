
#include "TerminalWindow.h"
#include "../shell/MultilineInput.h"

#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <sstream>
#include <system_error>
#include <algorithm>

#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <cerrno>
#include <cstdio>
#include <string>
#include <vector>
#include <signal.h>


TerminalWindow::TerminalWindow()
    : display(nullptr), window(0), gc(0) {
    int wakePipe[2];
    if (pipe(wakePipe) == -1) {
        perror("pipe");
    } else {
        wakeReadFd = wakePipe[0];
        wakeWriteFd = wakePipe[1];
        for (int fd : wakePipe) {
            int flags = fcntl(fd, F_GETFL, 0);
            if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
            int fdflags = fcntl(fd, F_GETFD, 0);
            if (fdflags >= 0) fcntl(fd, F_SETFD, fdflags | FD_CLOEXEC);
        }
    }
    createTab();
}

TerminalWindow::~TerminalWindow() {
    // Workers must finish before the wake-up pipe is closed.
    tabs.clear();
    retiredTabs.clear();
    if (wakeReadFd >= 0) close(wakeReadFd);
    if (wakeWriteFd >= 0) close(wakeWriteFd);
    if (display) {
        if (gc) XFreeGC(display, gc);
        if (window) XDestroyWindow(display, window);
        XCloseDisplay(display);
    }
}

Tab& TerminalWindow::currentTab() {
    return *tabs[activeTab];
}

void TerminalWindow::scroll(int amount) {
    Tab& tab = currentTab();

    const int visibleRows = 20;

    // Include the current prompt as one display row.
    int totalRows = static_cast<int>(tab.lines.size()) + 1;

    int maxOffset = std::max(0, totalRows - visibleRows);

    tab.scrollOffset = std::clamp(
        tab.scrollOffset + amount,
        0,
        maxOffset
    );

    redraw();
}


void TerminalWindow::notifyGui() const {
    if (wakeWriteFd < 0) return;
    const unsigned char signal = 1;
    // Nonblocking: EAGAIN means the pipe already contains a wake notification.
    ssize_t result;
    do { result = write(wakeWriteFd, &signal, 1); }
    while (result < 0 && errno == EINTR);
}

void TerminalWindow::createTab() {
    auto tab = std::make_unique<Tab>();
    tab->setGuiNotifier([this] { notifyGui(); });
    tabs.push_back(std::move(tab));
    activeTab = static_cast<int>(tabs.size()) - 1;
}

void TerminalWindow::switchTab() {
    activeTab = (activeTab + 1) % static_cast<int>(tabs.size());
}

void TerminalWindow::closeTab() {
    // For now, don't destroy a tab with a running child process.
    if (currentTab().executor.hasJobs() ||
        !currentTab().commandQueue.empty() ||
        currentTab().hasPendingSubmission() ||
        currentTab().isProcessingCommand() ||
        currentTab().isReadingOutput()) {
        currentTab().lines.push_back(
            "Wait for the command to finish before closing this tab."
        );
        return;
    }

    if (tabs.size() == 1) {
        currentTab().input.clear();
        currentTab().pendingOutput.clear();
        currentTab().commandQueue.clear();
        currentTab().lines.clear();
        currentTab().lines.push_back("Welcome to Terminator!");
        currentTab().scrollOffset = 0;
        return;
    }

    // Do not destroy/join the worker while holding stateMutex:
    // the worker may be waiting for that same mutex.
    retiredTabs.push_back(std::move(tabs[activeTab]));
    tabs.erase(tabs.begin() + activeTab);

    if (activeTab >= static_cast<int>(tabs.size()))
        activeTab = static_cast<int>(tabs.size()) - 1;
    
}

bool TerminalWindow::initialize() {
    display = XOpenDisplay(nullptr);

    if (!display) {
        fprintf(stderr, "Cannot open X display\n");
        return false;
    }

    int screen = DefaultScreen(display);

    window = XCreateSimpleWindow(
        display,
        RootWindow(display, screen),
        100, 100, 900, 550, 1,
        BlackPixel(display, screen),
        WhitePixel(display, screen)
    );

    if (!window) {
        fprintf(stderr, "Cannot create X window\n");
        return false;
    }

    XSelectInput(
    display,
    window,
    ExposureMask | KeyPressMask | ButtonPressMask );

    gc = XCreateGC(display, window, 0, nullptr);

    if (!gc) {
        fprintf(stderr, "Cannot create graphics context\n");
        return false;
    }

    XMapWindow(display, window);
    XFlush(display);

    return true;
}

void TerminalWindow::redraw() {
    XClearWindow(display, window);

    // Draw tabs.
    int x = 20;

    for (int i = 0; i < static_cast<int>(tabs.size()); i++) {
        std::string label = (i == activeTab ? "[ " : "  ");
        label += "Tab " + std::to_string(i + 1);
        label += (i == activeTab ? " ]" : "  ");

        XDrawString(
            display, window, gc,
            x, 25,
            label.c_str(),
            static_cast<int>(label.size())
        );

        x += 100;
    }

    XDrawLine(display, window, gc, 10, 38, 880, 38);

    Tab& tab = currentTab();

    const int visibleRows = 20;
    const int lineHeight = 22;
    const int topY = 65;

    int totalRows = static_cast<int>(tab.lines.size()) + 1;
    int maxOffset = std::max(0, totalRows - visibleRows);

    tab.scrollOffset = std::clamp(
        tab.scrollOffset, 0, maxOffset
    );

    int start = std::max(
        0, totalRows - visibleRows - tab.scrollOffset
    );

    int end = std::min(totalRows, start + visibleRows);

    int y = topY;

    for (int i = start; i < end; i++) {
        std::string line;

        if (i < static_cast<int>(tab.lines.size())) {
            line = tab.lines[i];
        } else {
            if (tab.executor.isRunning()) {
                line = "[command running...]";
            } else {
                line = (tab.multilinePending.empty() ?
                       "user@terminator:" + tab.shell.getCwd().filename().string() + "> " :
                       "> ") + tab.input + "_";
            }
        }

        XDrawString(
            display, window, gc,
            20, y,
            line.c_str(),
            static_cast<int>(line.size())
        );

        y += lineHeight;
    }

    // Draw scrollbar track.
    const int trackX = 880;
    const int trackY = 48;
    const int trackWidth = 10;
    const int trackHeight = 477;

    XDrawRectangle(
        display, window, gc,
        trackX, trackY,
        trackWidth, trackHeight
    );

    if (totalRows > visibleRows) {
        int thumbHeight = std::max(
            20,
            trackHeight * visibleRows / totalRows
        );

        thumbHeight = std::min(thumbHeight, trackHeight);

        int availableTravel = trackHeight - thumbHeight;

        int thumbY = trackY;

        if (maxOffset > 0) {
            thumbY += availableTravel *
                      (maxOffset - tab.scrollOffset) /
                      maxOffset;
        }

        XFillRectangle(
            display, window, gc,
            trackX + 2,
            thumbY + 2,
            trackWidth - 3,
            std::max(1, thumbHeight - 3)
        );
    }

    XFlush(display);
}
void TerminalWindow::redrawInputLine() { // optimization
    Tab& tab = currentTab();

    const int visibleRows = 20;
    const int lineHeight = 22;
    const int topY = 65;

    int totalRows = static_cast<int>(tab.lines.size()) + 1;

    int start = std::max(0, totalRows - visibleRows);

    int promptRow = totalRows - 1;

    int y = topY + (promptRow - start) * lineHeight;

    // Clear only the prompt row, not the entire window.
    XClearArea(
        display,
        window,
        20,
        y - 17,
        850,
        lineHeight,
        False
    );

    std::string prompt =
        (tab.multilinePending.empty() ?
        "user@terminator:" + tab.shell.getCwd().filename().string() + "> " :
        "> ") + tab.input + "_";

    XDrawString(
        display,
        window,
        gc,
        20,
        y,
        prompt.c_str(),
        static_cast<int>(prompt.size())
    );

    XFlush(display);
}



void TerminalWindow::appendOutput(
    Tab& tab, const std::string& text
) {
    tab.pendingOutput += text;

    size_t pos;

    while ((pos = tab.pendingOutput.find('\n')) != std::string::npos) {
        std::string line = tab.pendingOutput.substr(0, pos);

        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        tab.lines.push_back(line);

        if (tab.scrollOffset > 0) {
            tab.scrollOffset++;
        }
        tab.pendingOutput.erase(0, pos + 1);
    }

    // The caller schedules a GUI wake-up; workers must never call X11.
}

void TerminalWindow::readCommandOutput(Tab& tab, int fd) {
    std::string output = tab.executor.readOutput(fd);
    if (!output.empty()) appendOutput(tab, output);
}

void TerminalWindow::updateJobs(Tab& tab) {
    for (const auto& event : tab.executor.update()) {
        if (!event.message.empty()) tab.lines.push_back(event.message);
        if (event.foregroundFinished) {
            if (!tab.pendingOutput.empty()) {
                tab.lines.push_back(tab.pendingOutput);
                tab.pendingOutput.clear();
            }
            tab.shell.setLastStatus(event.status);
            scheduleCommandProcessing(tab);
        }
    }
}

void TerminalWindow::scheduleOutputRead(Tab& tab) {
    // GUI thread holds stateMutex. Coalesce readiness notifications per tab.
    if (!tab.beginOutputRead()) return;
    Tab* target = &tab;
    tab.enqueueWorkerTask([this, target] {
        bool changed = false;
        {
            std::lock_guard<std::mutex> guard(stateMutex);
            // Discover live descriptors inside the lock; a previously-polled
            // descriptor may already have closed or been reused.
            for (int fd : target->executor.outputFds()) {
                const size_t oldLines = target->lines.size();
                const size_t oldPending = target->pendingOutput.size();
                readCommandOutput(*target, fd);
                if (oldLines != target->lines.size() ||
                    oldPending != target->pendingOutput.size()) changed = true;
            }
            // EOF may permit the executor to reap and dispatch queued commands.
            const size_t before = target->lines.size();
            updateJobs(*target);
            if (before != target->lines.size()) changed = true;
            target->endOutputRead();
        }
        if (changed) notifyGui();
    });
}

void TerminalWindow::scheduleJobCheck(Tab& tab) {
    // Called with stateMutex held by the GUI thread.
    // Coalesce requests so the worker queue cannot grow without bound.
    if (!tab.beginJobCheck()) return;
    Tab* target = &tab;
    tab.enqueueWorkerTask([this, target] {
        bool changed = false;
        {
            std::lock_guard<std::mutex> guard(stateMutex);
            const size_t oldLines = target->lines.size();
            const bool wasRunning = target->executor.isRunning();
            updateJobs(*target);
            changed = oldLines != target->lines.size() ||
                      wasRunning != target->executor.isRunning();
            target->endJobCheck();
        }
        if (changed) notifyGui();
    });
}

void TerminalWindow::scheduleCommandProcessing(Tab& tab) {
    // Caller owns stateMutex; worker will acquire it before touching shell state.
    if (tab.executor.isRunning() || tab.commandQueue.empty() ||
        !tab.beginCommandProcessing()) return;

    Tab* target = &tab;
    tab.enqueueWorkerTask([this, target] {
        {
            std::lock_guard<std::mutex> guard(stateMutex);
            processCommandQueue(*target);
            target->endCommandProcessing();
        }
        notifyGui();
    });
}

void TerminalWindow::processCommandQueue(Tab& tab) {
    // A tab runs one command at a time. External commands resume the queue
    // when their output pipe reaches EOF in readCommandOutput().
    while (!tab.executor.isRunning() && !tab.commandQueue.empty()) {
        Tab::QueuedCommand item = tab.commandQueue.front();
        tab.commandQueue.pop_front();

        const std::string command = tab.shell.expandExitStatus(item.text);
        if (command.find_first_not_of(" \t\r") == std::string::npos)
            continue;

        commandHistory.push_back(item.text);
        tab.lines.push_back("user@terminator:" +
            tab.shell.getCwd().filename().string() + "> " + command);

        std::istringstream stream(command);
        std::string name;
        stream >> name;

        if (name == "cd") {
            std::string destination, error;
            stream >> destination;
            if (!tab.shell.changeDirectory(destination, error))
                tab.lines.push_back(error);
        }
        else if (name == "history") {
            for (size_t i = 0; i < commandHistory.size(); ++i)
                tab.lines.push_back(std::to_string(i + 1) + "  " + commandHistory[i]);
            tab.shell.setLastStatus(0);
        }
        else if (name == "jobs") {
            auto list = tab.executor.jobs();
            if (list.empty()) tab.lines.push_back("No jobs.");
            else for (const auto& line : list) tab.lines.push_back(line);
            tab.shell.setLastStatus(0);
        }
        else if (name == "fg" || name == "bg") {
            std::string arg, extra, message;
            stream >> arg;
            int id = 0;
            bool valid = !(stream >> extra);
            if (!arg.empty()) {
                if (arg[0] == '%') arg.erase(0, 1);
                valid = valid && !arg.empty() &&
                    std::all_of(arg.begin(), arg.end(), [](unsigned char c) { return c >= '0' && c <= '9'; });
                if (valid) {
                    try { id = std::stoi(arg); }
                    catch (...) { valid = false; }
                }
            }
            if (!valid) {
                tab.lines.push_back(name + ": usage: " + name + " [%job_id]");
                tab.shell.setLastStatus(1);
            } else {
                bool ok = name == "fg" ? tab.executor.foreground(id, message)
                                       : tab.executor.background(id, message);
                tab.lines.push_back(message);
                tab.shell.setLastStatus(ok ? 0 : 1);
            }
        }
        else if (name == "multiWatch") {
            std::vector<std::string> commands;
            std::string rest;
            std::getline(stream, rest);
            size_t pos = rest.find_first_not_of(" \t");
            bool invalid = pos == std::string::npos || rest[pos] != '[';
            if (!invalid) {
                ++pos;
                while (true) {
                    pos = rest.find_first_not_of(" \t", pos);
                    if (pos == std::string::npos) { invalid = true; break; }
                    if (rest[pos] == ']') {
                        ++pos;
                        if (rest.find_first_not_of(" \t", pos) != std::string::npos) invalid = true;
                        break;
                    }
                    if (rest[pos] != '"') { invalid = true; break; }
                    ++pos;
                    std::string command;
                    bool closed = false;
                    while (pos < rest.size()) {
                        char c = rest[pos++];
                        if (c == '"') { closed = true; break; }
                        if (c == '\\' && pos < rest.size() && rest[pos] == '"') c = rest[pos++];
                        command.push_back(c);
                    }
                    if (!closed || command.empty()) { invalid = true; break; }
                    commands.push_back(command);
                    pos = rest.find_first_not_of(" \t", pos);
                    if (pos == std::string::npos) { invalid = true; break; }
                    if (rest[pos] == ',') { ++pos; continue; }
                    if (rest[pos] == ']') {
                        ++pos;
                        if (rest.find_first_not_of(" \t", pos) != std::string::npos) invalid = true;
                        break;
                    }
                    invalid = true; break;
                }
            }
            if (invalid || commands.empty()) {
                tab.lines.push_back("multiWatch: usage: multiWatch [\"command1\", \"command2\"]");
                tab.shell.setLastStatus(1);
            } else if (!tab.executor.startMultiWatch(commands, tab.shell.getCwd().string())) {
                tab.lines.push_back("multiWatch: unable to start");
                tab.shell.setLastStatus(1);
            }
        }
        else if (name == "source") {
            std::string filename, extra;
            stream >> filename;
            if (filename.empty() || (stream >> extra)) {
                tab.lines.push_back("source: usage: source filename");
                tab.shell.setLastStatus(1);
                continue;
            }
            if (item.sourceDepth >= 8) {
                tab.lines.push_back("source: maximum nesting depth (8) exceeded");
                tab.shell.setLastStatus(1);
                continue;
            }
            std::filesystem::path filePath(filename);
            if (filePath.is_relative())
                filePath = tab.shell.getCwd() / filePath;
            std::ifstream file(filePath);
            if (!file) {
                tab.lines.push_back("source: cannot open " + filename);
                tab.shell.setLastStatus(1);
                continue;
            }
            std::vector<std::string> scriptLines;
            std::string line;
            while (std::getline(file, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                const size_t first = line.find_first_not_of(" \t");
                if (first == std::string::npos || line[first] == '#') continue;
                scriptLines.push_back(line);
            }
            if (file.bad()) {
                tab.lines.push_back("source: error reading " + filename);
                tab.shell.setLastStatus(1);
                continue;
            }
            // Insert script commands ahead of later queued commands.
            for (auto it = scriptLines.rbegin(); it != scriptLines.rend(); ++it)
                tab.commandQueue.push_front({*it, item.sourceDepth + 1});
            if (scriptLines.empty()) tab.shell.setLastStatus(0);
        }
        else if (name == "exit") {
            // In a sourced file, exit stops the script. At the interactive
            // prompt it is handled separately in handleKeyPress().
            tab.commandQueue.clear();
            tab.shell.setLastStatus(0);
        }
        else {
            std::string external = command;
            bool background = false;
            size_t last = external.find_last_not_of(" \t");
            if (last != std::string::npos && external[last] == '&') {
                background = true;
                external.erase(last);
            }
            if (!tab.executor.start(external, tab.shell.getCwd().string(), background)) {
                tab.lines.push_back("Failed to start command.");
                tab.shell.setLastStatus(1);
            } else if (background) {
                tab.shell.setLastStatus(0);
            }
        }
    }
}

void TerminalWindow::handleKeyPress(XKeyEvent& event) {
    KeySym key;
    char buffer[32];
    int count = XLookupString(&event, buffer, sizeof(buffer), &key, nullptr);

    if (event.state & ControlMask) {
        if (key == XK_c || key == XK_C) {
            if (currentTab().executor.signalForeground(SIGINT)) {
                currentTab().lines.push_back("^C");
                redraw();
            } else {
                currentTab().input.clear(); currentTab().multilinePending.clear();
                currentTab().browsingHistory = false; redraw();
            }
            return;
        }
        if (key == XK_z || key == XK_Z) {
            if (currentTab().executor.signalForeground(SIGTSTP)) {
                currentTab().lines.push_back("^Z"); redraw();
            }
            return;
        }
        if (key == XK_t || key == XK_T) {
            createTab(); redraw(); return;
        }
        if (key == XK_Tab) {
            switchTab(); redraw(); return;
        }
        if (key == XK_w || key == XK_W) {
            closeTab(); redraw(); return;
        }
    }

    Tab& tab = currentTab();
    if (tab.executor.isRunning() || !tab.commandQueue.empty() ||
        tab.hasPendingSubmission()) return;
    // Navigate the shared command history with Up/Down arrows.
    // Preserve what the user was typing before entering history browsing.
    if ((key == XK_Up || key == XK_Down) && tab.multilinePending.empty()) {
        if (key == XK_Up && !commandHistory.empty()) {
            if (!tab.browsingHistory) {
                tab.historyDraft = tab.input;
                tab.historyPosition = commandHistory.size();
                tab.browsingHistory = true;
            }
            if (tab.historyPosition > 0) --tab.historyPosition;
            tab.input = commandHistory[tab.historyPosition];
        } else if (key == XK_Down && tab.browsingHistory) {
            if (tab.historyPosition + 1 < commandHistory.size()) {
                ++tab.historyPosition;
                tab.input = commandHistory[tab.historyPosition];
            } else {
                tab.input = tab.historyDraft;
                tab.browsingHistory = false;
            }
        }
        tab.scrollOffset = 0;
        redraw();
        return;
    }
    bool wasScrolled = (tab.scrollOffset != 0);
    tab.scrollOffset = 0;

    if (key == XK_BackSpace) {
        if (!tab.input.empty()) {
            tab.input.pop_back();
        }

        if (wasScrolled)
            redraw();
        else
            redrawInputLine();

        return;
    }
    else if (key == XK_Return || key == XK_KP_Enter) {
        std::string rawCommand = tab.multilinePending;
        tab.browsingHistory = false;
        tab.historyDraft.clear();
        MultilineInput::appendLine(rawCommand, tab.input);
        tab.input.clear();
        if (MultilineInput::needsMore(rawCommand)) {
            tab.multilinePending = std::move(rawCommand);
            redraw();
            return;
        }
        tab.multilinePending.clear();
        std::istringstream input(rawCommand);
        std::string name;
        input >> name;
        if (name == "exit") {
            if (rawCommand.find_first_not_of(" \t") != std::string::npos)
                commandHistory.push_back(rawCommand);
            closeTab();
            redraw();
            return;
        }
        tab.submitCommand(rawCommand);
    }
    else if (count > 0) {
        for (int i = 0; i < count; ++i) {
            unsigned char ch = buffer[i];

            if (ch >= 32 && ch <= 126)
                tab.input += static_cast<char>(ch);
        }

        if (wasScrolled)
            redraw();
        else
            redrawInputLine();

        return;
    }
    redraw();
}

void TerminalWindow::run() {
    bool running = true;

    while (running) {
        std::unique_lock<std::mutex> stateLock(stateMutex);
        // Handle queued X11 events.
        while (XPending(display) > 0) {
            XEvent event;
            XNextEvent(display, &event);

            if (event.type == Expose &&
                event.xexpose.count == 0) {
                redraw();
            }

            else if (event.type == ButtonPress) {
                if (event.xbutton.button == Button4) {
                    scroll(3);   // Scroll up three lines
                }
                else if (event.xbutton.button == Button5) {
                    scroll(-3);  // Scroll down three lines
                }
            }

            else if (event.type == KeyPress) {
                KeySym key = XLookupKeysym(&event.xkey, 0);

                if (key == XK_Escape) {
                    bool anyRunning = false;

                    for (const auto& tab : tabs) {
                        if (tab->executor.hasJobs() || !tab->commandQueue.empty() ||
                            tab->hasPendingSubmission() ||
                            tab->isProcessingCommand() || tab->isReadingOutput()) {
                            anyRunning = true;
                            break;
                        }
                    }

                    if (anyRunning) {
                        currentTab().lines.push_back(
                            "Wait for commands to finish before exiting."
                        );
                        redraw();
                    } else {
                        running = false;
                        break;
                    }
                } else {
                    handleKeyPress(event.xkey);
                }
            }
        }

        if (!running)
            break;

        // Collect prepared commands from each tab's worker on the GUI thread.
        // No worker directly touches the GUI, shell state or command queue.
        for (auto& tab : tabs) {
            for (std::string& command : tab->takeSubmittedCommands())
                tab->commandQueue.push_back({std::move(command), 0});
            if (!tab->commandQueue.empty() && !tab->executor.isRunning())
                scheduleCommandProcessing(*tab);
        }

        // Poll X11 and output pipes for all foreground/background jobs.
        std::vector<pollfd> fds;
        std::vector<Tab*> monitored;
        pollfd xfd{};
        xfd.fd = ConnectionNumber(display);
        xfd.events = POLLIN;
        fds.push_back(xfd);
        if (wakeReadFd >= 0) {
            pollfd wakeFd{};
            wakeFd.fd = wakeReadFd;
            wakeFd.events = POLLIN;
            fds.push_back(wakeFd);
        }
        const size_t outputStart = fds.size();
        for (auto& tab : tabs) {
            for (int fdNumber : tab->executor.outputFds()) {
                pollfd fd{};
                fd.fd = fdNumber;
                fd.events = POLLIN | POLLHUP;
                fds.push_back(fd);
                monitored.push_back(tab.get());
            }
        }
        // A bounded timeout allows waitpid(WNOHANG|WUNTRACED) to detect
        // stopped/exited children even when their output pipe stays open.
        stateLock.unlock();
        // Joining a retired worker under stateMutex deadlocks when that
        // worker is waiting to acquire the mutex.
        retiredTabs.clear();
        int result = poll(fds.data(), fds.size(), 10);
        stateLock.lock();
        if (result < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }
        if (wakeReadFd >= 0 && (fds[1].revents & (POLLIN | POLLHUP))) {
            unsigned char buffer[128];
            while (read(wakeReadFd, buffer, sizeof(buffer)) > 0) {}
        }
        for (size_t i = outputStart; i < fds.size(); ++i) {
            if (fds[i].revents & (POLLIN | POLLHUP | POLLERR))
                scheduleOutputRead(*monitored[i - outputStart]);
        }
        // waitpid/job-status checks now run on the owning tab worker.
        for (auto& tab : tabs) scheduleJobCheck(*tab);
        if (result > 0) redraw();
    }
}
