#pragma once

#include <queue>
#include <mutex>
#include <functional>
#include <thread>

#include <remidy/priv/event-loop.hpp>
#include <remidy-gui/remidy-gui.hpp>

namespace uapmd::service::gui {

class ImGuiEventLoop : public remidy::EventLoop {
private:
    std::queue<std::function<void()>> taskQueue_;
    std::mutex queueMutex_;
    std::thread::id mainThreadId_{std::this_thread::get_id()};
    bool running_ = false;

protected:
    void initializeOnUIThreadImpl() override {
        // nothing extra for ImGui loop
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

    bool running() const {
        return running_;
    }
};

} // namespace uapmd::service::gui
