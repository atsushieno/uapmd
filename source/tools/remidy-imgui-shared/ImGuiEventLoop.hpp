#pragma once

#include <queue>
#include <mutex>
#include <functional>
#include <thread>
#if defined(__ANDROID__)
#include <android/log.h>
#endif

#include <remidy/detail/event-loop.hpp>
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
        mainThreadId_ = std::this_thread::get_id();
    }

    bool runningOnMainThreadImpl() override {
        return std::this_thread::get_id() == mainThreadId_;
    }

    void enqueueTaskOnMainThreadImpl(std::function<void()>&& func) override {
        std::lock_guard<std::mutex> lock(queueMutex_);
        taskQueue_.push(std::move(func));
#if defined(__ANDROID__)
        __android_log_print(ANDROID_LOG_INFO, "uapmd-adb",
                            "ImGuiEventLoop: queued task, size=%zu",
                            taskQueue_.size());
#endif
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
#if defined(__ANDROID__)
        if (!localQueue.empty()) {
            __android_log_print(ANDROID_LOG_INFO, "uapmd-adb",
                                "ImGuiEventLoop: draining %zu task(s)",
                                localQueue.size());
        }
#endif
        while (!localQueue.empty()) {
            auto task = std::move(localQueue.front());
            localQueue.pop();
#if !defined(__ANDROID__)
            remidy::gui::GLContextGuard glGuard;
#endif
            task();
        }
    }

    bool isRunning() const {
        return running_;
    }
};

} // namespace uapmd::gui
