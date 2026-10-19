#include "ProcessChannel.h"
#include <iostream>
#include <string>
static bool run(ProcessChannel& p, const std::string& cmd, std::string& out) {
    return p.send("CMD " + cmd) && p.receive(out);
}
int main() {
    ProcessChannel a, b;
    if (!a.start() || !b.start()) return 1;
    std::string x,y;
    if (!run(a, "echo child-one", x) || x.find("child-one") == std::string::npos) return 2;
    if (!run(b, "echo child-two", y) || y.find("child-two") == std::string::npos) return 3;
    if (!run(a, "cd /", x) || x != "/") return 4;
    if (!run(a, "pwd", x) || x != "/") return 5;
    std::cout << "PROCESS COMMAND PASS: independent children executed echo and cd/pwd\\n";
}
