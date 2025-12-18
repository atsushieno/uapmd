#pragma once

#include <queue>
#include <mutex>
#include <functional>
#include <thread>

#include <remidy/priv/event-loop.hpp>
#include <remidy-gui/remidy-gui.hpp>

namespace uapmd::gui {

/**
 * ImGui-compatible event loop for Remidy.
 * Processes tasks queued from audio threads safely on the UI thread.
 * It used to be shared by remidy-plugin-host, uapmd-service, and uapmd-app to avoid code duplication.
 */
class ImGuiEventLoop : public remidy::EventLoop {
private:
    std::queue<std::function<void()>> taskQueue_;
    std::mutex queueMutex_;
    std::thread::id mainThreadId_;
    bool running_ = false;

protected:
    void initializeOnUIThreadImpl() override {
        // ImGui loop is already initialized on main thread
    }

    bool runningOnMainThreadImpl() override {
        return std::this_thread::get_id() == mainThreadId_;
    }

    void enqueueTaskOnMainThreadImpl(std::function<void()>&& func) override {
        std::lock_guard<std::mutex> lock(queueMutex_);
        taskQueue_.push(std::move(func));
    }

    void startImpl() override {
        running_ = true;
    }

    void stopImpl() override {
        running_ = false;
    }

public:
    ImGuiEventLoop() : mainThreadId_(std::this_thread::get_id()) {}

    /**
     * Process all queued tasks. Call this in the main render loop.
     * This swaps the queue before processing to minimize lock contention.
     */
    void processQueuedTasks() {
        std::queue<std::function<void()>> localQueue;
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            std::swap(localQueue, taskQueue_);
        }
        while (!localQueue.empty()) {
            auto task = std::move(localQueue.front());
            localQueue.pop();
            remidy::gui::GLContextGuard glGuard;
            task();
        }
    }

    bool isRunning() const {
        return running_;
    }
};

} // namespace uapmd::gui
