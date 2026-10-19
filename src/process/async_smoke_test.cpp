#include "AsyncProcessChannel.h"
#include <chrono>
#include <iostream>
#include <thread>

int main() {
    AsyncProcessChannel a, b;
    if (!a.start() || !b.start() || a.pid() == b.pid()) return 1;
    if (!a.submit("sleep 1") || !b.submit("echo ready")) return 2;
    bool fastReply = false, slowReply = false;
    auto begin = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - begin < std::chrono::seconds(5)) {
        for (auto& reply : b.takeReplies()) {
            if (reply.second.find("ready") != std::string::npos &&
                std::chrono::steady_clock::now() - begin < std::chrono::milliseconds(800))
                fastReply = true;
        }
        if (!a.takeReplies().empty()) slowReply = true;
        if (fastReply && slowReply) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!fastReply || !slowReply) return 3;
    std::cout << "ASYNC IPC PASS: one child responds while another runs sleep\\n";
}
