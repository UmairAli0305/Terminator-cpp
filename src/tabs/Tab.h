
#ifndef TAB_H
#define TAB_H
#include "../shell/ShellSession.h"
#include "../shell/CommandExecutor.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <filesystem>
#include <string>
#include <vector>

class Tab {
private:
    std::thread worker;
    std::mutex taskMutex;
    std::condition_variable taskReady;
    std::deque<std::function<void()>> tasks;
    bool stopping = false;
    std::mutex submittedMutex;
    std::deque<std::string> submittedCommands;
    std::atomic<unsigned> pendingSubmissions{0};
    std::atomic<bool> processingCommand{false};
    std::atomic<bool> checkingJobs{false};
    std::function<void()> notifyGui;

    void workerLoop() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(taskMutex);
                taskReady.wait(lock, [this] {
                    return stopping || !tasks.empty();
                });
                if (stopping && tasks.empty()) return;
                task = std::move(tasks.front());
                tasks.pop_front();
            }
            task();
        }
    }

public:
    // The worker does not access X11. Only the GUI thread may draw.
    void enqueueWorkerTask(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lock(taskMutex);
            if (stopping) return;
            tasks.push_back(std::move(task));
        }
        taskReady.notify_one();
    }

    // Set before submitting commands; callback must be safe from worker threads.
    void setGuiNotifier(std::function<void()> callback) {
        notifyGui = std::move(callback);
    }

    // GUI submits text; the per-tab worker prepares the command for dispatch.
    // Only the GUI thread later touches commandQueue, ShellSession or X11.
    void submitCommand(std::string text) {
        pendingSubmissions.fetch_add(1);
        enqueueWorkerTask([this, text = std::move(text)]() mutable {
            // Normalize line endings off the GUI thread.
            while (!text.empty() && (text.back() == '\r' || text.back() == '\n'))
                text.pop_back();
            {
                std::lock_guard<std::mutex> lock(submittedMutex);
                submittedCommands.push_back(std::move(text));
            }
            if (notifyGui) notifyGui();
        });
    }

    std::vector<std::string> takeSubmittedCommands() {
        std::vector<std::string> result;
        std::lock_guard<std::mutex> lock(submittedMutex);
        while (!submittedCommands.empty()) {
            result.push_back(std::move(submittedCommands.front()));
            submittedCommands.pop_front();
        }
        pendingSubmissions.fetch_sub(static_cast<unsigned>(result.size()));
        return result;
    }

    bool hasPendingSubmission() const { return pendingSubmissions.load() != 0; }

    // Only one command-dispatch task may be scheduled per tab.
    bool beginCommandProcessing() {
        bool expected = false;
        return processingCommand.compare_exchange_strong(expected, true);
    }
    void endCommandProcessing() { processingCommand.store(false); }
    bool isProcessingCommand() const { return processingCommand.load(); }

    bool beginJobCheck() {
        bool expected = false;
        return checkingJobs.compare_exchange_strong(expected, true);
    }
    void endJobCheck() { checkingJobs.store(false); }

    std::string input;
    std::string pendingOutput;
    std::vector<std::string> lines;
    CommandExecutor executor;
    int scrollOffset = 0;
    ShellSession shell;

    struct QueuedCommand {
        std::string text;
        int sourceDepth = 0;
    };
    std::deque<QueuedCommand> commandQueue;
    

    Tab() {
        lines.push_back("Welcome to Terminator!");
        worker = std::thread(&Tab::workerLoop, this);
    }

    Tab(const Tab&) = delete;
    Tab& operator=(const Tab&) = delete;

    ~Tab() {
        {
            std::lock_guard<std::mutex> lock(taskMutex);
            stopping = true;
        }
        taskReady.notify_one();
        if (worker.joinable()) worker.join();
    }
};

#endif
