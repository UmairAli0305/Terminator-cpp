#include "gui/TerminalWindow.h"

int main() {
    TerminalWindow terminal;

    if (!terminal.initialize()) {
        return 1;
    }

    terminal.run();

    return 0;
}
