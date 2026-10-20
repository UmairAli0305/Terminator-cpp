#ifndef ASYNC_PROCESS_CHANNEL_H
#define ASYNC_PROCESS_CHANNEL_H

#include "ProcessChannel.h"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

// Parent-side asynchronous request/reply transport.
// No X11 calls here; GUI can collect completed replies using takeReplies().
class AsyncProcessChannel {
    ProcessChannel channel;
    std::thread ioThread;
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<std::string> requests;
    std::deque<std::pair<std::string, std::string>> replies;
    bool stopping = false;
    bool failed = false;
    void run();
public:
    AsyncProcessChannel() = default;
    AsyncProcessChannel(const AsyncProcessChannel&) = delete;
    AsyncProcessChannel& operator=(const AsyncProcessChannel&) = delete;
    ~AsyncProcessChannel();
    bool start();
    bool startProcess(); // fork before any GUI/IO threads exist
    bool startReader();  // start the communication thread afterwards
    bool submit(std::string command);
    bool interrupt();
    std::deque<std::pair<std::string, std::string>> takeReplies();
    bool hasFailed();
    pid_t pid() const { return channel.pid(); }
};
#endif
