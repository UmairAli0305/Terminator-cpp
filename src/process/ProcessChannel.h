#ifndef PROCESS_CHANNEL_H
#define PROCESS_CHANNEL_H
#include <string>
#include <sys/types.h>

class ProcessChannel {
    pid_t childPid = -1;
    int commandWrite = -1;
    int replyRead = -1;
public:
    ProcessChannel() = default;
    ProcessChannel(const ProcessChannel&) = delete;
    ProcessChannel& operator=(const ProcessChannel&) = delete;
    ~ProcessChannel();
    bool start();
    bool send(const std::string& text);
    bool receive(std::string& text);
    void stop();
    pid_t pid() const { return childPid; }
};
#endif
