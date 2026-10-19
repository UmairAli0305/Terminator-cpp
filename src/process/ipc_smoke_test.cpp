#include "ProcessChannel.h"
#include <iostream>
int main() {
    ProcessChannel tab;
    if (!tab.start()) return 1;
    std::string received;
    if (!tab.send("hello from GUI") || !tab.receive(received)) return 2;
    if (received != "hello from GUI") return 3;
    std::cout << "IPC PASS: child PID " << tab.pid() << ", reply: " << received << "\\n";
    return 0;
}
