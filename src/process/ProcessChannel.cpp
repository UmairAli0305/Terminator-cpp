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
volatile sig_atomic_t suspendRequested = 0;
extern "C" void processInterrupt(int) { interruptRequested = 1; }
extern "C" void processSuspend(int) { suspendRequested = 1; }

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
        action.sa_handler = processSuspend;
        sigaction(SIGUSR2, &action, nullptr);
        ShellSession shell;
        // One executor per tab process: jobs survive individual IPC requests.
        CommandExecutor executor;
        std::string request;
        while (readFrame(toChild[0], request)) {
            // P2: Each child owns an independent shell working directory.
            // Legacy raw messages still echo for the P1 transport test.
            std::string reply = request;
            if (request.rfind("CMD ", 0) == 0) {
                interruptRequested = 0;
                suspendRequested = 0;
                const std::string command = request.substr(4);
                if (command.rfind("multiWatch", 0) == 0) {
                    std::vector<std::string> commands;
                    if (!parseMultiWatch(command, commands)) {
                        if (!sendFrame(fromChild[1], "multiWatch: usage: multiWatch [\"cmd1\", \"cmd2\"]")) break;
                        continue;
                    }
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
                // Collect completed background jobs on every new request.
                std::string notices;
                for (const auto& event : executor.update())
                    if (!event.message.empty()) notices += event.message + "\n";
                const auto commandText = request.substr(4);
                std::istringstream input(commandText);
                std::string operation;
                input >> operation;
                if (operation == "pwd") {
                    reply = notices + shell.getCwd().string();
                } else if (operation == "cd") {
                    std::string path, error, extra;
                    input >> path;
                    if (input >> extra) reply = "cd: too many arguments";
                    else if (!shell.changeDirectory(path, error)) reply = error;
                    else reply = shell.getCwd().string();
                    reply = notices + reply;
                } else if (operation == "jobs") {
                    auto jobs = executor.jobs();
                    reply = notices;
                    if (jobs.empty()) reply += "No jobs.\n";
                    else for (const auto& job : jobs) reply += job + "\n";
                } else if (operation == "bg" || operation == "fg") {
                    std::string target, extra, message;
                    input >> target;
                    if (input >> extra) reply = operation + ": too many arguments";
                    else {
                        int id = 0;
                        if (!target.empty()) {
                            if (target[0] == '%') target.erase(0, 1);
                            if (target.empty() || target.find_first_not_of("0123456789") != std::string::npos)
                                id = -1;
                            else {
                                try { id = std::stoi(target); }
                                catch (...) { id = -1; }
                            }
                        }
                        bool ok = operation == "bg" ? executor.background(id, message)
                                                      : executor.foreground(id, message);
                        reply = message;
                        if (ok && operation == "fg") {
                            reply += "\n";
                            // Foreground jobs are handled by the same signal-aware wait below.
                        }
                    }
                    reply = notices + reply;
                } else if (!operation.empty()) {
                    std::string executable = commandText;
                    size_t end = executable.find_last_not_of(" \t\r\n");
                    bool background = end != std::string::npos && executable[end] == '&';
                    if (background) {
                        executable.erase(end);
                        end = executable.find_last_not_of(" \t\r\n");
                        executable.erase(end == std::string::npos ? 0 : end + 1);
                    }
                    if (executable.empty() || !executor.start(executable, shell.getCwd().string(), background))
                        reply = notices + "Failed to start command.";
                    else reply = notices + (background ? "[background] " + executable + "\n" : "");
                } else reply = notices;

                // Wait only for a foreground command; retain stopped/background jobs.
                // Poll with a bounded timeout so SIGUSR1/SIGUSR2 are handled promptly.
                if (executor.isRunning()) {
                    bool running = true;
                    while (running) {
                        if (interruptRequested) {
                            executor.signalForeground(SIGINT);
                            interruptRequested = 0;
                        }
                        if (suspendRequested) {
                            executor.signalForeground(SIGTSTP);
                            suspendRequested = 0;
                        }
                        auto fds = executor.outputFds();
                        std::vector<pollfd> watched;
                        for (int fd : fds) {
                            pollfd p{}; p.fd = fd; p.events = POLLIN | POLLHUP;
                            watched.push_back(p);
                        }
                        int ready = poll(watched.data(), watched.size(), 50);
                        if (ready < 0 && errno != EINTR) break;
                        if (ready > 0) for (const auto& p : watched)
                            if (p.revents & (POLLIN | POLLHUP | POLLERR))
                                reply += executor.readOutput(p.fd);
                        for (const auto& event : executor.update())
                            if (!event.message.empty()) reply += event.message + "\n";
                        running = executor.isRunning();
                        if (reply.size() > 60000) {
                            reply += "\n[output truncated]\n";
                            executor.signalForeground(SIGINT);
                            break;
                        }
                    }
                }
                // Drain available output from background jobs without blocking.
                if (!executor.isRunning()) {
                    auto fds = executor.outputFds();
                    for (int fd : fds) reply += executor.readOutput(fd);
                    for (const auto& event : executor.update())
                        if (!event.message.empty()) reply += event.message + "\n";
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
