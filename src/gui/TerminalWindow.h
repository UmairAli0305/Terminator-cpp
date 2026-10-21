
#ifndef TERMINAL_WINDOW_H
#define TERMINAL_WINDOW_H

#include <X11/Xlib.h>
#include "MouseSelection.h"

#include "../tabs/Tab.h"

#include <memory>
class X11Clipboard;
#include <mutex>
#include <string>
#include <vector>

class TerminalWindow {
private:
    Display* display;
    Window window;
    GC gc;
    std::unique_ptr<X11Clipboard> clipboard;
    MouseSelection selection;
    XFontStruct* selectionFont = nullptr;
    int selectionTab = -1;
    // Protects shell/executor/command queues and GUI-visible tab state.
    std::mutex stateMutex;
    int wakeReadFd = -1;
    int wakeWriteFd = -1;
    void notifyGui() const;

    std::vector<std::unique_ptr<Tab>> tabs;
    // Closed tabs are joined only while stateMutex is unlocked.
    std::vector<std::unique_ptr<Tab>> retiredTabs;
    int activeTab = 0;

    void scroll(int amount); //scrolling function

    void redraw();
    void redrawInputLine();
    void handleKeyPress(XKeyEvent& event);
    void appendOutput(Tab& tab, const std::string& text);
    void readCommandOutput(Tab& tab, int fd);
    void updateJobs(Tab& tab);
    void scheduleJobCheck(Tab& tab);
    void scheduleOutputRead(Tab& tab);
    void processCommandQueue(Tab& tab);
    void scheduleCommandProcessing(Tab& tab);

    void createTab();
    void switchTab();
    void closeTab();
    std::vector<std::string> commandHistory;

    Tab& currentTab();

public:
    TerminalWindow();
    ~TerminalWindow();

    bool initialize();
    void run();
};

#endif
