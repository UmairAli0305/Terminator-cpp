#include "CommandExecutor.h"
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>
#include <cerrno>
#include <cstdio>
#include <sstream>
#include <algorithm>
#include <poll.h>
#include <cstring>

// Task 4: parse redirection operators without passing them to execvp.
// This is intentionally not the full Task 3 quoting/expansion parser.
namespace {
struct Redirection {
    std::vector<std::string> args;
    std::string input, output, error;
};

Redirection parseRedirection(const std::string& command) {
    Redirection result;
    std::vector<std::string> tokens;
    std::string word;
    char quote = 0;
    bool started = false;
    auto flush = [&]() {
        if (started) { tokens.push_back(word); word.clear(); started = false; }
    };
    for (size_t i = 0; i < command.size(); ++i) {
        char c = command[i];
        if (quote) {
            if (c == quote) { quote = 0; continue; }
            if (quote == '"' && c == '\\' && i + 1 < command.size() &&
                (command[i + 1] == '"' || command[i + 1] == '\\')) {
                word += command[++i];
            } else word += c;
            started = true;
        } else if (c == '\'' || c == '"') {
            quote = c; started = true;
        } else if (c == '\\' && i + 1 < command.size()) {
            word += command[++i]; started = true;
        } else if (c == '<' || c == '>') {
            flush(); tokens.emplace_back(std::string(1, '\x1f') + c);
        } else if (c == ' ' || c == '\t' || c == '\n') {
            flush();
        } else {
            word += c; started = true;
        }
    }
    if (quote) { result.error = "unterminated quote"; return result; }
    flush();
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (tokens[i] == "\x1f<" || tokens[i] == "\x1f>") {
            bool input = tokens[i] == "\x1f<";
            if (++i >= tokens.size() || tokens[i] == "\x1f<" || tokens[i] == "\x1f>") {
                result.error = "missing filename after redirection"; return result;
            }
            (input ? result.input : result.output) = tokens[i];
        } else result.args.push_back(tokens[i]);
    }
    if (result.args.empty()) result.error = "missing command";
    return result;
}
}

CommandExecutor::~CommandExecutor() { shutdown(); }

bool CommandExecutor::start(const std::string& command, const std::string& cwd, bool background) {
    if (!background && isRunning()) return false;
    Redirection redirect = parseRedirection(command);
    if (!redirect.error.empty()) {
        fprintf(stderr, "terminator: %s\n", redirect.error.c_str());
        return false;
    }
    std::vector<char*> argv;
    for (auto& a : redirect.args) argv.push_back(a.data());
    argv.push_back(nullptr);
    int fds[2];
    if (pipe(fds) == -1) { perror("pipe"); return false; }
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); close(fds[0]); close(fds[1]); return false; }
    if (pid == 0) {
        close(fds[0]);
        setpgid(0, 0);
        if (chdir(cwd.c_str()) == -1) { perror("chdir"); _exit(127); }
        if (dup2(fds[1], STDOUT_FILENO) == -1 || dup2(fds[1], STDERR_FILENO) == -1) _exit(127);
        close(fds[1]);
        if (!redirect.input.empty()) {
            int inputFd = open(redirect.input.c_str(), O_RDONLY);
            if (inputFd == -1) {
                dprintf(STDERR_FILENO, "terminator: %s: %s\n",
                        redirect.input.c_str(), strerror(errno));
                _exit(1);
            }
            if (dup2(inputFd, STDIN_FILENO) == -1) { perror("dup2 stdin"); _exit(1); }
            close(inputFd);
        }
        if (!redirect.output.empty()) {
            int outputFd = open(redirect.output.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
            if (outputFd == -1) {
                dprintf(STDERR_FILENO, "terminator: %s: %s\n",
                        redirect.output.c_str(), strerror(errno));
                _exit(1);
            }
            if (dup2(outputFd, STDOUT_FILENO) == -1) { perror("dup2 stdout"); _exit(1); }
            close(outputFd);
        }
        execvp(argv[0], argv.data());
        if (errno == ENOENT) dprintf(STDERR_FILENO, "terminator: %s: command not found\n", argv[0]);
        else perror("execvp");
        _exit(errno == ENOENT ? 127 : 126);
    }
    close(fds[1]);
    // Parent also sets PGID to avoid a race with a quick signal.
    if (setpgid(pid, pid) == -1 && errno != EACCES && errno != ESRCH) perror("setpgid");
    int flags = fcntl(fds[0], F_GETFL);
    if (flags != -1) fcntl(fds[0], F_SETFL, flags | O_NONBLOCK);
    active.push_back({nextId++, pid, fds[0], command, State::Running, !background});
    return true;
}

// One-shot multiWatch coordinator. The coordinator and its commands share a
// process group, so the existing Ctrl+C/Ctrl+Z job-control code works.
bool CommandExecutor::startMultiWatch(const std::vector<std::string>& commands,
                                     const std::string& cwd) {
    if (commands.empty() || isRunning()) return false;
    int outputPipe[2];
    if (pipe(outputPipe) == -1) { perror("pipe"); return false; }
    pid_t coordinator = fork();
    if (coordinator == -1) {
        perror("fork"); close(outputPipe[0]); close(outputPipe[1]); return false;
    }
    if (coordinator == 0) {
        close(outputPipe[0]);
        setpgid(0, 0);
        if (chdir(cwd.c_str()) == -1) { perror("chdir"); _exit(1); }
        if (dup2(outputPipe[1], STDOUT_FILENO) == -1 ||
            dup2(outputPipe[1], STDERR_FILENO) == -1) _exit(1);
        close(outputPipe[1]);

        struct Child { pid_t pid; int fd; std::string name; bool open; };
        std::vector<Child> children;
        for (const std::string& cmd : commands) {
            std::istringstream words(cmd);
            std::vector<std::string> args;
            std::string word;
            while (words >> word) args.push_back(word);
            if (args.empty()) continue;
            int p[2];
            if (pipe(p) == -1) { perror("multiWatch pipe"); continue; }
            pid_t pid = fork();
            if (pid == -1) { perror("multiWatch fork"); close(p[0]); close(p[1]); continue; }
            if (pid == 0) {
                close(p[0]);
                if (dup2(p[1], STDOUT_FILENO) == -1 ||
                    dup2(p[1], STDERR_FILENO) == -1) _exit(1);
                close(p[1]);
                std::vector<char*> argv;
                for (auto& arg : args) argv.push_back(arg.data());
                argv.push_back(nullptr);
                execvp(argv[0], argv.data());
                if (errno == ENOENT)
                    dprintf(STDERR_FILENO, "terminator: %s: command not found\n", argv[0]);
                else perror("execvp");
                _exit(errno == ENOENT ? 127 : 126);
            }
            close(p[1]);
            children.push_back({pid, p[0], cmd, true});
        }
        dprintf(STDOUT_FILENO, "[multiWatch] Monitoring %zu commands\n", children.size());
        size_t openCount = children.size();
        while (openCount > 0) {
            std::vector<pollfd> fds;
            std::vector<size_t> indexes;
            for (size_t i = 0; i < children.size(); ++i) {
                if (children[i].open) {
                    pollfd p{}; p.fd = children[i].fd; p.events = POLLIN | POLLHUP;
                    fds.push_back(p); indexes.push_back(i);
                }
            }
            int ready = poll(fds.data(), fds.size(), -1);
            if (ready < 0) { if (errno == EINTR) continue; break; }
            for (size_t i = 0; i < fds.size(); ++i) {
                if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
                Child& child = children[indexes[i]];
                char buffer[4096];
                ssize_t n = read(child.fd, buffer, sizeof(buffer));
                if (n > 0) {
                    dprintf(STDOUT_FILENO, "[%s] ", child.name.c_str());
                    // Output can contain embedded newlines; print as one chunk.
                    ssize_t written = 0;
                    while (written < n) {
                        ssize_t w = write(STDOUT_FILENO, buffer + written, static_cast<size_t>(n - written));
                        if (w < 0 && errno == EINTR) continue;
                        if (w <= 0) break;
                        written += w;
                    }
                    if (buffer[n - 1] != '\n') dprintf(STDOUT_FILENO, "\n");
                } else if (n == 0 || (n < 0 && errno != EINTR)) {
                    close(child.fd); child.open = false; --openCount;
                }
            }
        }
        int finalStatus = 0;
        for (auto& child : children) {
            if (child.open) close(child.fd);
            int status = 0;
            pid_t result;
            do { result = waitpid(child.pid, &status, 0); } while (result == -1 && errno == EINTR);
            if (result == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) finalStatus = 1;
        }
        dprintf(STDOUT_FILENO, "[multiWatch] All commands completed\n");
        _exit(finalStatus);
    }
    close(outputPipe[1]);
    if (setpgid(coordinator, coordinator) == -1 && errno != EACCES && errno != ESRCH)
        perror("setpgid");
    int flags = fcntl(outputPipe[0], F_GETFL);
    if (flags != -1) fcntl(outputPipe[0], F_SETFL, flags | O_NONBLOCK);
    active.push_back({nextId++, coordinator, outputPipe[0], "multiWatch", State::Running, true});
    return true;
}

bool CommandExecutor::isRunning() const {
    for (const auto& j : active) if (j.foreground && j.state == State::Running) return true;
    return false;
}
bool CommandExecutor::hasJobs() const { return !active.empty(); }
std::vector<int> CommandExecutor::outputFds() const {
    std::vector<int> fds;
    for (const auto& j : active) if (j.fd >= 0) fds.push_back(j.fd);
    return fds;
}
std::string CommandExecutor::readOutput(int fd) {
    for (auto& j : active) if (j.fd == fd) {
        std::string output;
        char buffer[4096];
        while (true) {
            ssize_t n = read(fd, buffer, sizeof(buffer));
            if (n > 0) output.append(buffer, static_cast<size_t>(n));
            else if (n == 0) { close(j.fd); j.fd = -1; break; }
            else if (errno == EINTR) continue;
            else if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            else { close(j.fd); j.fd = -1; break; }
        }
        return output;
    }
    return "";
}
std::vector<CommandExecutor::Event> CommandExecutor::update() {
    std::vector<Event> events;
    for (auto& j : active) {
        if (j.reaped) continue;
        int status = 0;
        pid_t result = waitpid(j.pid, &status, WNOHANG | WUNTRACED | WCONTINUED);
        if (result == 0) continue;
        if (result < 0) {
            if (errno != ECHILD) continue;
            j.reaped = true; j.state = State::Finished; j.status = 1;
        } else if (WIFSTOPPED(status)) {
            j.state = State::Stopped;
            bool wasForeground = j.foreground;
            j.foreground = false;
            events.push_back({"[" + std::to_string(j.id) + "] Stopped  " + j.command,
                              wasForeground, 128 + WSTOPSIG(status)});
        } else if (WIFCONTINUED(status)) {
            j.state = State::Running;
        } else if (WIFEXITED(status) || WIFSIGNALED(status)) {
            j.reaped = true; j.state = State::Finished;
            j.status = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        }
    }
    for (auto it = active.begin(); it != active.end();) {
        if (it->reaped && it->fd == -1) {
            bool fg = it->foreground;
            events.push_back({fg ? "" : "[" + std::to_string(it->id) + "] Done  " + it->command,
                              fg, it->status});
            it = active.erase(it);
        } else ++it;
    }
    return events;
}
CommandExecutor::Job* CommandExecutor::find(int id) {
    if (id == 0) {
        for (auto it = active.rbegin(); it != active.rend(); ++it)
            if (it->state != State::Finished && !it->foreground) return &*it;
        return nullptr;
    }
    for (auto& j : active) if (j.id == id && j.state != State::Finished) return &j;
    return nullptr;
}
std::vector<std::string> CommandExecutor::jobs() const {
    std::vector<std::string> result;
    for (const auto& j : active) if (j.state != State::Finished)
        result.push_back("[" + std::to_string(j.id) + "] " +
            (j.state == State::Stopped ? "Stopped  " : "Running  ") + j.command);
    return result;
}
bool CommandExecutor::background(int id, std::string& message) {
    Job* j = find(id);
    if (!j) { message = "bg: no such job"; return false; }
    if (j->state == State::Stopped && kill(-j->pid, SIGCONT) == -1) { message = "bg: SIGCONT failed"; return false; }
    j->state = State::Running; j->foreground = false;
    message = "[" + std::to_string(j->id) + "] " + j->command;
    return true;
}
bool CommandExecutor::foreground(int id, std::string& message) {
    if (isRunning()) { message = "fg: another foreground job is running"; return false; }
    Job* j = find(id);
    if (!j) { message = "fg: no such job"; return false; }
    if (j->state == State::Stopped && kill(-j->pid, SIGCONT) == -1) { message = "fg: SIGCONT failed"; return false; }
    j->state = State::Running; j->foreground = true;
    message = j->command;
    return true;
}
bool CommandExecutor::signalForeground(int sig) {
    for (auto& j : active) if (j.foreground && j.state == State::Running)
        return kill(-j.pid, sig) == 0;
    return false;
}
void CommandExecutor::shutdown() {
    for (auto& j : active) {
        if (!j.reaped) {
            kill(-j.pid, SIGKILL);
            int status;
            while (waitpid(j.pid, &status, 0) == -1 && errno == EINTR) {}
        }
        if (j.fd >= 0) close(j.fd);
    }
    active.clear();
}
