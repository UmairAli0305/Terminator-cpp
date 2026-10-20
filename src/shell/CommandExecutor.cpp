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
#include <cctype>
#include <cstdlib>

// Task 4: parse redirection operators without passing them to execvp.
// This is intentionally not the full Task 3 quoting/expansion parser.
namespace {
struct Redirection {
    std::vector<std::string> args;
    std::string input, output, error;
};

// Execute a substitution using Terminator's own parser and execvp, never a shell.
// Stage 3 supports simple commands and nested substitutions (pipelines follow later).
static Redirection parseRedirection(const std::string& command, const std::string& cwd);
// Split a substitution into pipeline stages without splitting inside quotes or nested $().
static bool splitPipeline(const std::string& expression, std::vector<std::string>& stages,
                          std::string& error) {
    char quote = 0;
    bool backtick = false;
    int nested = 0;
    size_t start = 0;
    for (size_t i = 0; i < expression.size(); ++i) {
        char c = expression[i];
        if (c == '\\' && quote != '\'' && i + 1 < expression.size()) { ++i; continue; }
        if (quote == '\'') { if (c == '\'') quote = 0; continue; }
        if (c == '\'') { quote = '\''; continue; }
        if (c == '`') { backtick = !backtick; continue; }
        if (backtick) continue;
        if (c == '"') { quote = quote == '"' ? 0 : '"'; continue; }
        if (c == '$' && i + 1 < expression.size() && expression[i+1] == '(') {
            ++nested; ++i; continue;
        }
        if (c == ')' && nested > 0) { --nested; continue; }
        if (c == '|' && nested == 0) {
            if (i + 1 < expression.size() && expression[i+1] == '|') {
                error = "logical OR (||) in substitutions is not supported"; return false;
            }
            stages.push_back(expression.substr(start, i - start));
            start = i + 1;
        }
    }
    stages.push_back(expression.substr(start));
    for (const auto& stage : stages) {
        if (stage.find_first_not_of(" \t\r\n") == std::string::npos) {
            error = "empty pipeline stage in substitution"; return false;
        }
    }
    return true;
}

static bool substitute(const std::string& expression, const std::string& cwd,
                       std::string& output, std::string& error) {
    std::vector<std::string> stages;
    if (!splitPipeline(expression, stages, error)) return false;
    std::vector<Redirection> commands;
    for (const auto& stage : stages) {
        Redirection parsed = parseRedirection(stage, cwd);
        if (!parsed.error.empty()) { error = parsed.error; return false; }
        commands.push_back(std::move(parsed));
    }
    int capture[2];
    if (pipe(capture) == -1) { error = strerror(errno); return false; }
    int previousRead = -1;
    std::vector<pid_t> children;
    for (size_t i = 0; i < commands.size(); ++i) {
        int next[2] = {-1, -1};
        if (i + 1 < commands.size() && pipe(next) == -1) {
            error = strerror(errno); break;
        }
        pid_t pid = fork();
        if (pid == -1) {
            error = strerror(errno);
            if (next[0] >= 0) close(next[0]);
            if (next[1] >= 0) close(next[1]);
            break;
        }
        if (pid == 0) {
            close(capture[0]);
            if (chdir(cwd.c_str()) == -1) _exit(127);
            if (previousRead >= 0 && dup2(previousRead, STDIN_FILENO) == -1) _exit(127);
            if (i + 1 < commands.size()) {
                close(next[0]);
                if (dup2(next[1], STDOUT_FILENO) == -1) _exit(127);
            } else if (dup2(capture[1], STDOUT_FILENO) == -1) _exit(127);
            if (previousRead >= 0) close(previousRead);
            if (next[1] >= 0) close(next[1]);
            close(capture[1]);
            const Redirection& cmd = commands[i];
            if (!cmd.input.empty()) {
                int fd = open(cmd.input.c_str(), O_RDONLY);
                if (fd == -1) { dprintf(STDERR_FILENO, "terminator: %s: %s\n", cmd.input.c_str(), strerror(errno)); _exit(1); }
                if (dup2(fd, STDIN_FILENO) == -1) _exit(1);
                close(fd);
            }
            if (!cmd.output.empty()) {
                int fd = open(cmd.output.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0666);
                if (fd == -1) _exit(1);
                if (dup2(fd, STDOUT_FILENO) == -1) _exit(1);
                close(fd);
            }
            std::vector<char*> argv;
            for (const auto& arg : cmd.args) argv.push_back(const_cast<char*>(arg.c_str()));
            argv.push_back(nullptr);
            execvp(argv[0], argv.data());
            dprintf(STDERR_FILENO, "terminator: substitution: %s: %s\n", argv[0], strerror(errno));
            _exit(127);
        }
        children.push_back(pid);
        if (previousRead >= 0) close(previousRead);
        if (next[1] >= 0) close(next[1]);
        previousRead = next[0];
    }
    if (previousRead >= 0) close(previousRead);
    close(capture[1]);
    char buf[4096];
    while (true) {
        ssize_t n = read(capture[0], buf, sizeof(buf));
        if (n == 0) break;
        if (n < 0) { if (errno == EINTR) continue; if (error.empty()) error = strerror(errno); break; }
        if (output.size() + static_cast<size_t>(n) > 1024 * 1024) {
            error = "command substitution output exceeds 1 MiB";
            break;
        }
        output.append(buf, static_cast<size_t>(n));
    }
    close(capture[0]);
    for (pid_t pid : children) {
        int status = 0;
        while (waitpid(pid, &status, 0) == -1 && errno == EINTR) {}
    }
    while (!output.empty() && output.back() == '\n') output.pop_back();
    return error.empty();
}
static bool extractSubstitution(const std::string& source, size_t& i,
                                std::string& expression, std::string& error) {
    const bool backtick = source[i] == '`';
    size_t start = i + (backtick ? 1 : 2);
    size_t pos = start;
    int depth = 1;
    char quote = 0;
    for (; pos < source.size(); ++pos) {
        char c = source[pos];
        if (c == '\\' && quote != '\'' && pos + 1 < source.size()) { ++pos; continue; }
        if (quote == '\'') { if (c == '\'') quote = 0; continue; }
        if (c == '\'') { quote = '\''; continue; }
        if (c == '"') { quote = quote == '"' ? 0 : '"'; continue; }
        if (backtick) { if (c == '`' && quote != '"') { expression = source.substr(start, pos-start); i = pos; return true; } }
        else {
            if (c == '$' && pos + 1 < source.size() && source[pos+1] == '(') { ++depth; ++pos; continue; }
            if (c == ')' && --depth == 0) { expression = source.substr(start, pos-start); i = pos; return true; }
        }
    }
    error = "unfinished command substitution";
    return false;
}
Redirection parseRedirection(const std::string& command, const std::string& cwd) {
    Redirection result;
    std::vector<std::string> tokens;
    std::string word;
    char quote = 0;
    bool started = false;
    auto flush = [&]() {
        if (started) { tokens.push_back(word); word.clear(); started = false; }
    };
    auto expansion = [&](size_t& i, bool quoted) -> bool {
        std::string expr, value;
        if (!extractSubstitution(command, i, expr, result.error)) return false;
        if (!substitute(expr, cwd, value, result.error)) return false;
        if (quoted) { word += value; started = true; }
        else {
            // Unquoted substitution: split on whitespace, preserving word boundaries.
            for (char c : value) {
                if (std::isspace(static_cast<unsigned char>(c))) {
                    if (started) { tokens.push_back(word); word.clear(); started = false; }
                } else { word += c; started = true; }
            }
        }
        return true;
    };
    auto variable = [&](size_t& i) -> std::string {
        size_t begin = i + 1;
        if (begin >= command.size()) return "$";
        if (command[begin] == '?') {
            ++i;
            return "$?"; // Existing ShellSession expands exit status earlier.
        }
        if (!(std::isalpha(static_cast<unsigned char>(command[begin])) ||
              command[begin] == '_')) return "$";
        size_t end = begin + 1;
        while (end < command.size() &&
               (std::isalnum(static_cast<unsigned char>(command[end])) || command[end] == '_')) ++end;
        std::string key = command.substr(begin, end - begin);
        i = end - 1;
        const char* value = std::getenv(key.c_str());
        return value ? value : "";
    };
    for (size_t i = 0; i < command.size(); ++i) {
        char c = command[i];
        if (quote == '\'') {
            if (c == '\'') quote = 0;
            else word += c;
            started = true;
            continue;
        }
        if (quote == '"') {
            if (c == '"') { quote = 0; continue; }
            if (c == '\\' && i + 1 < command.size()) {
                char next = command[i + 1];
                if (next == '"' || next == '\\' || next == '$' || next == '`') {
                    word += next; ++i; started = true; continue;
                }
                if (next == '\n') { ++i; continue; }
            }
            if ((c == '$' && i + 1 < command.size() && command[i+1] == '(') || c == '`') {
                if (!expansion(i, true)) return result;
            } else if (c == '$') word += variable(i);
            else word += c;
            started = true;
            continue;
        }
        if (c == '\'' || c == '"') { quote = c; started = true; }
        else if (c == '\\' && i + 1 < command.size()) {
            if (command[i + 1] != '\n') { word += command[++i]; started = true; }
            else ++i;
        }
        else if ((c == '$' && i + 1 < command.size() && command[i+1] == '(') || c == '`') {
            if (!expansion(i, false)) return result;
        }
        else if (c == '$') { word += variable(i); started = true; }
        else if (c == '<' || c == '>') {
            flush(); tokens.emplace_back(std::string(1, '\x1f') + c);
        }
        else if (c == ' ' || c == '\t' || c == '\n') flush();
        else { word += c; started = true; }
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
    // Task 5: ordinary top-level pipelines (not only $(...) substitutions).
    // A coordinator owns the pipeline children and belongs to their process group.
    std::vector<std::string> stages;
    std::string pipelineError;
    if (!splitPipeline(command, stages, pipelineError)) {
        fprintf(stderr, "terminator: %s\n", pipelineError.c_str());
        return false;
    }
    if (stages.size() > 1) {
        std::vector<Redirection> parsed;
        for (const auto& stage : stages) {
            Redirection r = parseRedirection(stage, cwd);
            if (!r.error.empty() || r.args.empty()) {
                fprintf(stderr, "terminator: %s\n", r.error.empty() ? "empty pipeline command" : r.error.c_str());
                return false;
            }
            parsed.push_back(std::move(r));
        }
        int capture[2];
        if (pipe(capture) == -1) { perror("pipe"); return false; }
        pid_t coordinator = fork();
        if (coordinator == -1) {
            perror("fork"); close(capture[0]); close(capture[1]); return false;
        }
        if (coordinator == 0) {
            close(capture[0]);
            setpgid(0, 0);
            if (chdir(cwd.c_str()) == -1) _exit(127);
            int previousRead = -1;
            std::vector<pid_t> children;
            bool failed = false;
            for (size_t i = 0; i < parsed.size(); ++i) {
                int next[2] = {-1, -1};
                if (i + 1 < parsed.size() && pipe(next) == -1) {
                    dprintf(capture[1], "terminator: pipe: %s\n", strerror(errno));
                    failed = true; break;
                }
                pid_t child = fork();
                if (child == -1) {
                    dprintf(capture[1], "terminator: fork: %s\n", strerror(errno));
                    if (next[0] >= 0) close(next[0]);
                    if (next[1] >= 0) close(next[1]);
                    failed = true; break;
                }
                if (child == 0) {
                    if (previousRead >= 0 && dup2(previousRead, STDIN_FILENO) == -1) _exit(127);
                    if (i + 1 < parsed.size()) {
                        close(next[0]);
                        if (dup2(next[1], STDOUT_FILENO) == -1) _exit(127);
                    } else if (dup2(capture[1], STDOUT_FILENO) == -1) _exit(127);
                    if (dup2(capture[1], STDERR_FILENO) == -1) _exit(127);
                    if (previousRead >= 0) close(previousRead);
                    if (next[1] >= 0) close(next[1]);
                    close(capture[1]);
                    const Redirection& r = parsed[i];
                    if (!r.input.empty()) {
                        int fd = open(r.input.c_str(), O_RDONLY);
                        if (fd == -1) {
                            dprintf(STDERR_FILENO, "terminator: %s: %s\n", r.input.c_str(), strerror(errno));
                            _exit(1);
                        }
                        if (dup2(fd, STDIN_FILENO) == -1) _exit(1);
                        close(fd);
                    }
                    if (!r.output.empty()) {
                        int fd = open(r.output.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
                        if (fd == -1) {
                            dprintf(STDERR_FILENO, "terminator: %s: %s\n", r.output.c_str(), strerror(errno));
                            _exit(1);
                        }
                        if (dup2(fd, STDOUT_FILENO) == -1) _exit(1);
                        close(fd);
                    }
                    std::vector<char*> argv;
                    for (const auto& a : r.args) argv.push_back(const_cast<char*>(a.c_str()));
                    argv.push_back(nullptr);
                    execvp(argv[0], argv.data());
                    dprintf(STDERR_FILENO, "terminator: %s: %s\n", argv[0], strerror(errno));
                    _exit(errno == ENOENT ? 127 : 126);
                }
                children.push_back(child);
                if (previousRead >= 0) close(previousRead);
                if (next[1] >= 0) close(next[1]);
                previousRead = next[0];
            }
            if (previousRead >= 0) close(previousRead);
            close(capture[1]);
            int result = failed ? 1 : 0;
            for (pid_t child : children) {
                int status = 0;
                pid_t w;
                do { w = waitpid(child, &status, 0); } while (w == -1 && errno == EINTR);
                if (w == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) result = 1;
            }
            _exit(result);
        }
        close(capture[1]);
        if (setpgid(coordinator, coordinator) == -1 && errno != EACCES && errno != ESRCH)
            perror("setpgid");
        int flags = fcntl(capture[0], F_GETFL);
        if (flags != -1) fcntl(capture[0], F_SETFL, flags | O_NONBLOCK);
        active.push_back({nextId++, coordinator, capture[0], command, State::Running, !background});
        return true;
    }
    Redirection redirect = parseRedirection(command, cwd);
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
