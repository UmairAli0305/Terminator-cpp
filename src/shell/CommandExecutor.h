#ifndef COMMAND_EXECUTOR_H
#define COMMAND_EXECUTOR_H
#include <string>
#include <vector>
#include <sys/types.h>

class CommandExecutor {
public:
    struct Event { std::string message; bool foregroundFinished = false; int status = 0; };
    ~CommandExecutor();
    bool start(const std::string& command, const std::string& cwd, bool background = false);
    bool startMultiWatch(const std::vector<std::string>& commands, const std::string& cwd);
    bool isRunning() const; // A foreground job is active (running or being resumed).
    bool hasJobs() const;
    std::vector<int> outputFds() const;
    std::string readOutput(int fd);
    std::vector<Event> update();
    std::vector<std::string> jobs() const;
    bool background(int id, std::string& message);
    bool foreground(int id, std::string& message);
    bool signalForeground(int sig);
    void shutdown();
private:
    enum class State { Running, Stopped, Finished };
    struct Job {
        int id; pid_t pid; int fd; std::string command;
        State state; bool foreground; bool reaped = false;
        int status = 0;
    };
    std::vector<Job> active;
    int nextId = 1;
    Job* find(int id);
};
#endif
