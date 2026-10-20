#include "AsyncProcessChannel.h"
#include <signal.h>

AsyncProcessChannel::~AsyncProcessChannel() {
    {
        std::lock_guard<std::mutex> lock(mutex);
        stopping = true;
    }
    ready.notify_one();
    // Current P3 child is synchronous. A running command must complete before join.
    if (ioThread.joinable()) ioThread.join();
}

bool AsyncProcessChannel::start() {
    return startProcess() && startReader();
}

bool AsyncProcessChannel::startProcess() { return channel.start(); }

bool AsyncProcessChannel::startReader() {
    if (channel.pid() <= 0 || ioThread.joinable()) return false;
    ioThread = std::thread(&AsyncProcessChannel::run, this);
    return true;
}

bool AsyncProcessChannel::submit(std::string command) {
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (stopping || failed) return false;
        requests.push_back(std::move(command));
    }
    ready.notify_one();
    return true;
}

void AsyncProcessChannel::run() {
    for (;;) {
        std::string command;
        {
            std::unique_lock<std::mutex> lock(mutex);
            ready.wait(lock, [this] { return stopping || !requests.empty(); });
            if (stopping && requests.empty()) break;
            command = std::move(requests.front());
            requests.pop_front();
        }
        std::string output;
        if (!channel.send("CMD " + command) || !channel.receive(output)) {
            std::lock_guard<std::mutex> lock(mutex);
            failed = true;
            break;
        }
        if (command.rfind("multiWatch", 0) == 0 && output.compare(0, 5, "\x01MORE", 5) == 0) {
            while (output.compare(0, 5, "\x01MORE", 5) == 0) {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    replies.emplace_back(command, output.substr(5));
                }
                if (!channel.receive(output)) {
                    std::lock_guard<std::mutex> lock(mutex);
                    failed = true;
                    return;
                }
            }
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            replies.emplace_back(std::move(command), output == std::string("\x01DONE", 5) ? std::string("\x01DONE", 5) : std::move(output));
        }
    }
}

std::deque<std::pair<std::string, std::string>> AsyncProcessChannel::takeReplies() {
    std::deque<std::pair<std::string, std::string>> result;
    std::lock_guard<std::mutex> lock(mutex);
    result.swap(replies);
    return result;
}

bool AsyncProcessChannel::hasFailed() {
    std::lock_guard<std::mutex> lock(mutex);
    return failed;
}

bool AsyncProcessChannel::interrupt() {
    return channel.pid() > 0 && kill(channel.pid(), SIGUSR1) == 0;
}
