#include "ProcessChannel.h"
#include "../shell/ShellSession.h"
#include "../shell/CommandExecutor.h"
#include <poll.h>
#include <sstream>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <vector>
#include <atomic>

namespace {
volatile sig_atomic_t interruptRequested = 0;
extern "C" void processInterrupt(int) { interruptRequested = 1; }

bool parseMultiWatch(const std::string& command, std::vector<std::string>& commands) {
    const std::string prefix = "multiWatch";
    if (command.compare(0, prefix.size(), prefix) != 0) return false;
    size_t pos = command.find_first_not_of(" \t", prefix.size());
    if (pos == std::string::npos || command[pos++] != '[') return false;
    for (;;) {
        pos = command.find_first_not_of(" \t", pos);
        if (pos == std::string::npos) return false;
        if (command[pos] == ']') {
            ++pos;
            return !commands.empty() && command.find_first_not_of(" \t", pos) == std::string::npos;
        }
        if (command[pos++] != '"') return false;
        std::string item;
        bool closed = false;
        while (pos < command.size()) {
            char c = command[pos++];
            if (c == '"') { closed = true; break; }
            if (c == '\\' && pos < command.size() && command[pos] == '"') c = command[pos++];
            item.push_back(c);
        }
        if (!closed || item.empty()) return false;
        commands.push_back(item);
        pos = command.find_first_not_of(" \t", pos);
        if (pos == std::string::npos) return false;
        if (command[pos] == ',') { ++pos; continue; }
        if (command[pos] != ']') return false;
    }
}

bool writeAll(int fd, const void* data, size_t length) {
    auto* p = static_cast<const char*>(data);
    while (length) {
        ssize_t n = write(fd, p, length);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += n; length -= static_cast<size_t>(n);
    }
    return true;
}
bool readAll(int fd, void* data, size_t length) {
    auto* p = static_cast<char*>(data);
    while (length) {
        ssize_t n = read(fd, p, length);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += n; length -= static_cast<size_t>(n);
    }
    return true;
}
bool sendFrame(int fd, const std::string& message) {
    if (message.size() > 65536) return false;
    uint32_t size = static_cast<uint32_t>(message.size());
    return writeAll(fd, &size, sizeof(size)) &&
           writeAll(fd, message.data(), message.size());
}
bool readFrame(int fd, std::string& message) {
    uint32_t size = 0;
    if (!readAll(fd, &size, sizeof(size)) || size > 65536) return false;
    message.resize(size);
    return readAll(fd, message.data(), size);
}
}
ProcessChannel::~ProcessChannel() { stop(); }
bool ProcessChannel::start() {
    if (childPid > 0) return false;
    int toChild[2], fromChild[2];
    if (pipe(toChild) != 0) return false;
    if (pipe(fromChild) != 0) {
        close(toChild[0]); close(toChild[1]); return false;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(toChild[0]); close(toChild[1]);
        close(fromChild[0]); close(fromChild[1]);
        return false;
    }
    if (pid == 0) {
        close(toChild[1]); close(fromChild[0]);
        // Close pipes inherited from previously forked tab children.
        // Without this, another child can keep a tab's command pipe open,
        // preventing clean shutdown when its parent closes the pipe.
        long limit = sysconf(_SC_OPEN_MAX);
        if (limit < 0 || limit > 65536) limit = 65536;
        for (int fd = 3; fd < limit; ++fd) {
            if (fd != toChild[0] && fd != fromChild[1]) close(fd);
        }
        // IPC transport test only; shell execution is added in a later stage.
        struct sigaction action{};
        action.sa_handler = processInterrupt;
        sigemptyset(&action.sa_mask);
        sigaction(SIGUSR1, &action, nullptr);
        ShellSession shell;
        std::string request;
        while (readFrame(toChild[0], request)) {
            // P2: Each child owns an independent shell working directory.
            // Legacy raw messages still echo for the P1 transport test.
            std::string reply = request;
            if (request.rfind("CMD ", 0) == 0) {
                interruptRequested = 0;
                const std::string command = request.substr(4);
                if (command.rfind("multiWatch", 0) == 0) {
                    std::vector<std::string> commands;
                    if (!parseMultiWatch(command, commands)) {
                        if (!sendFrame(fromChild[1], "multiWatch: usage: multiWatch [\"cmd1\", \"cmd2\"]")) break;
                        continue;
                    }
                    CommandExecutor executor;
                    if (!executor.startMultiWatch(commands, shell.getCwd().string())) {
                        if (!sendFrame(fromChild[1], "multiWatch: unable to start")) break;
                        continue;
                    }
                    bool connected = true;
                    while (executor.hasJobs() && !interruptRequested && connected) {
                        auto fds = executor.outputFds();
                        std::vector<pollfd> watched;
                        for (int fd : fds) { pollfd p{}; p.fd = fd; p.events = POLLIN | POLLHUP; watched.push_back(p); }
                        int ready = poll(watched.data(), watched.size(), 100);
                        if (ready < 0 && errno != EINTR) break;
                        if (ready > 0) for (const auto& entry : watched) {
                            if (entry.revents & (POLLIN | POLLHUP | POLLERR)) {
                                std::string chunk = executor.readOutput(entry.fd);
                                // Send chunks immediately, not after multiWatch exits.
                                for (size_t i = 0; i < chunk.size(); i += 32000) {
                                    if (!sendFrame(fromChild[1], std::string("\x01MORE", 5) + chunk.substr(i, 32000))) {
                                        connected = false; break;
                                    }
                                }
                            }
                        }
                        executor.update();
                    }
                    if (interruptRequested) executor.signalForeground(SIGINT);
                    executor.shutdown();
                    interruptRequested = 0;
                    if (!connected || !sendFrame(fromChild[1], std::string("\x01DONE", 5))) break;
                    continue;
                }
                std::istringstream input(request.substr(4));
                std::string operation;
                input >> operation;
                if (operation == "pwd") {
                    reply = shell.getCwd().string();
                } else if (operation == "cd") {
                    std::string path, error, extra;
                    input >> path;
                    if (input >> extra) {
                        reply = "cd: too many arguments";
                    } else if (!shell.changeDirectory(path, error)) {
                        reply = error;
                    } else {
                        reply = shell.getCwd().string();
                    }
                } else if (!operation.empty()) {
                    CommandExecutor executor;
                    if (!executor.start(request.substr(4), shell.getCwd().string())) {
                        reply = "Failed to start command.";
                    } else {
                        reply.clear();
                        // This synchronous transport is a P3 test harness only.
                        // GUI integration will require asynchronous event frames.
                        bool running = true;
                        while (running) {
                            auto outputFds = executor.outputFds();
                            std::vector<pollfd> watched;
                            for (int fd : outputFds) {
                                pollfd entry{};
                                entry.fd = fd;
                                entry.events = POLLIN | POLLHUP;
                                watched.push_back(entry);
                            }
                            if (!watched.empty()) {
                                int ready;
                                do { ready = poll(watched.data(), watched.size(), 50); }
                                while (ready < 0 && errno == EINTR);
                                if (ready < 0) break;
                                for (const auto& entry : watched) {
                                    if (entry.revents & (POLLIN | POLLHUP | POLLERR))
                                        reply += executor.readOutput(entry.fd);
                                }
                            } else {
                                usleep(10000);
                            }
                            for (const auto& event : executor.update()) {
                                if (!event.message.empty()) reply += event.message + "\\n";
                            }
                            running = executor.hasJobs();
                            if (reply.size() > 60000) {
                                reply += "\\n[output truncated]\\n";
                                executor.shutdown();
                                break;
                            }
                        }
                    }
                }
            }
            if (!sendFrame(fromChild[1], reply)) break;
        }
        close(toChild[0]); close(fromChild[1]);
        _exit(0);
    }
    close(toChild[0]); close(fromChild[1]);
    childPid = pid;
    commandWrite = toChild[1];
    replyRead = fromChild[0];
    return true;
}
bool ProcessChannel::send(const std::string& text) {
    return commandWrite >= 0 && sendFrame(commandWrite, text);
}
bool ProcessChannel::receive(std::string& text) {
    return replyRead >= 0 && readFrame(replyRead, text);
}
void ProcessChannel::stop() {
    if (commandWrite >= 0) { close(commandWrite); commandWrite = -1; }
    if (replyRead >= 0) { close(replyRead); replyRead = -1; }
    if (childPid > 0) {
        int status;
        while (waitpid(childPid, &status, 0) < 0 && errno == EINTR) {}
        childPid = -1;
    }
}
