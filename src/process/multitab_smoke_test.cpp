#include "ProcessChannel.h"
#include <iostream>
#include <string>
#include <unistd.h>

static bool request(ProcessChannel& channel, const std::string& command,
                    std::string& reply) {
    return channel.send("CMD " + command) && channel.receive(reply);
}

int main() {
    ProcessChannel first, second;
    if (!first.start() || !second.start()) return 1;
    if (first.pid() == second.pid()) return 2;
    std::string original, secondCwd, firstCwd;
    if (!request(first, "pwd", original)) return 3;
    if (!request(first, "cd /", firstCwd) || firstCwd != "/") return 4;
    if (!request(second, "pwd", secondCwd)) return 5;
    if (secondCwd != original) return 6;
    if (!request(first, "pwd", firstCwd) || firstCwd != "/") return 7;
    std::cout << "MULTITAB IPC PASS: separate child processes and independent cwd\\n";
    return 0;
}
