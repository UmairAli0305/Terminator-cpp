#include "gui/TerminalWindow.h"
#include <cstdio>
#include <cstring>

int runProcessWindow();

int main(int argc, char* argv[]) {
    if (argc > 2 || (argc == 2 && std::strcmp(argv[1], "useProcess=True") != 0)) {
        std::fprintf(stderr, "Usage: %s [useProcess=True]\n", argv[0]);
        return 2;
    }
    if (argc == 2) return runProcessWindow();
    TerminalWindow terminal;
    if (!terminal.initialize()) return 1;
    terminal.run();
    return 0;
}
