
#ifndef TERMINAL_WINDOW_H
#define TERMINAL_WINDOW_H

#include <X11/Xlib.h>

#include "../tabs/Tab.h"

#include <memory>
#include <string>
#include <vector>

class TerminalWindow {
private:
    Display* display;
    Window window;
    GC gc;

    std::vector<std::unique_ptr<Tab>> tabs;
    int activeTab = 0;

    void scroll(int amount); //scrolling function

    void redraw();
    void redrawInputLine();
    void handleKeyPress(XKeyEvent& event);
    void appendOutput(Tab& tab, const std::string& text);
    void readCommandOutput(Tab& tab, int fd);
    void updateJobs(Tab& tab);
    void processCommandQueue(Tab& tab);

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
