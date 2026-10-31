#include "gui/TerminalWindow.h"
#include "shell/SharedHistory.h"
#include <cstdio>
#include <cstring>

int runProcessWindow();

int main(int argc, char* argv[]) {
    if (argc > 2 || (argc == 2 && std::strcmp(argv[1], "useProcess=True") != 0)) {
        std::fprintf(stderr, "Usage: %s [useProcess=True]\n", argv[0]);
        return 2;
    }
    if (!SharedHistory::initialize()) {
        std::fprintf(stderr, "terminator: shared history initialization failed\n");
        return 1;
    }
    int result = 0;
    if (argc == 2) {
        result = runProcessWindow();
    } else {
        TerminalWindow terminal;
        if (!terminal.initialize()) result = 1;
        else terminal.run();
    }
    SharedHistory::shutdown();
    return result;
}
