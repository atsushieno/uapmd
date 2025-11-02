#pragma once

#include <remidy/priv/event-loop.hpp>
#include <remidy-gui/remidy-gui.hpp>
#include <queue>
#include <mutex>
#include <functional>
#include <thread>

namespace uapmd::gui {
    class ImGuiEventLoop : public remidy::EventLoop {
    private:
        std::queue<std::function<void()>> taskQueue_;
        std::mutex queueMutex_;
        std::thread::id mainThreadId_;
        bool isRunning_ = false;

    public:
        ImGuiEventLoop() : mainThreadId_(std::this_thread::get_id()) {}

    protected:
        void initializeOnUIThreadImpl() override {
            // Already initialized in main thread
        }

        bool runningOnMainThreadImpl() override {
            return std::this_thread::get_id() == mainThreadId_;
        }

        void enqueueTaskOnMainThreadImpl(std::function<void()>&& func) override {
            std::lock_guard<std::mutex> lock(queueMutex_);
            taskQueue_.push(std::move(func));
        }

        void startImpl() override {
            isRunning_ = true;
        }

        void stopImpl() override {
            isRunning_ = false;
        }

    public:
        // Call this in the main loop to process queued tasks
        void processQueuedTasks() {
            std::lock_guard<std::mutex> lock(queueMutex_);
            while (!taskQueue_.empty()) {
                auto task = std::move(taskQueue_.front());
                taskQueue_.pop();
                remidy::gui::GLContextGuard glGuard;
                task();
            }
        }

        bool isRunning() const { return isRunning_; }
    };
}
