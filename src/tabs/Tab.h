
#ifndef TAB_H
#define TAB_H
#include "../shell/ShellSession.h"
#include "../shell/CommandExecutor.h"

#include <deque>
#include <filesystem>
#include <string>
#include <vector>

class Tab {
public:
    std::string input;
    std::string pendingOutput;
    std::vector<std::string> lines;
    CommandExecutor executor;
    int scrollOffset = 0;
    ShellSession shell;

    struct QueuedCommand {
        std::string text;
        int sourceDepth = 0;
    };
    std::deque<QueuedCommand> commandQueue;
    

    Tab() {
    lines.push_back("Welcome to Terminator!");
    }
};

#endif
