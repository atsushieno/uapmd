#include <assert.h>
#include <iostream>
#include <queue>
#include <mutex>
#if ANDROID
#include <android/looper.h>
#elif defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#elif (defined(__linux__) || defined(__unix__)) && !defined(__EMSCRIPTEN__)
#include "EventLoopLinux.hpp"
#else
#include <choc/gui/choc_MessageLoop.h>
#endif
#include "remidy/remidy.hpp"

#if ANDROID
#include "AndroidUiBridge.hpp"
#endif

namespace remidy {
#if ANDROID
    EventLoop* eventLoop{getEventLoop()};

    class EventLoopAndroid : public EventLoop {
        protected:
        void initializeOnUIThreadImpl() override {
        }
        bool runningOnMainThreadImpl() override {
            // SDL3 main() runs on SDLThread, but we want UI tasks to run on Android UI thread.
            // For now, we assume if we are on SDLThread we might need to delegate,
            // or we can just check if we are on the actual UI thread.
            // But runTaskOnMainThread uses this to decide if it should run immediately.
            // If we want JNI calls to work, they MUST be on a thread with the class loader.
            // SDLThread has it. Android UI thread has it.
            // Let's just always delegate to Android UI thread for now if we want to be safe.
            return false;
        }
        void enqueueTaskOnMainThreadImpl(std::function<void()>&& func) override {
            runOnAndroidUiThread(std::move(func));
        }
        void startImpl() override {
        }

        void stopImpl() override {
        }
    };
    EventLoopAndroid androidEventLoop{};
    EventLoop* getEventLoop() {
        if (!eventLoop)
            eventLoop = &androidEventLoop;
        return eventLoop;
    }
#elif defined(__EMSCRIPTEN__)
    class EventLoopEmscripten : public EventLoop {
    public:
        EventLoopEmscripten() = default;

    protected:
        void initializeOnUIThreadImpl() override {
            running_ = true;
        }

        bool runningOnMainThreadImpl() override {
            return true;
        }

        void enqueueTaskOnMainThreadImpl(std::function<void()>&& func) override {
            {
                std::lock_guard<std::mutex> lock(queueMutex_);
                taskQueue_.emplace(std::move(func));
            }
            emscripten_async_call(&EventLoopEmscripten::drainTasksThunk, this, 0);
        }

        void startImpl() override {
            running_ = true;
        }

        void stopImpl() override {
            running_ = false;
        }

    private:
        static void drainTasksThunk(void* ctx) {
            static_cast<EventLoopEmscripten*>(ctx)->drainTasks();
        }

        void drainTasks() {
            std::queue<std::function<void()>> localQueue;
            {
                std::lock_guard<std::mutex> lock(queueMutex_);
                std::swap(localQueue, taskQueue_);
            }
            while (!localQueue.empty()) {
                auto task = std::move(localQueue.front());
                localQueue.pop();
                if (task)
                    task();
            }
        }

        std::mutex queueMutex_;
        std::queue<std::function<void()>> taskQueue_;
        bool running_{false};
    };

    EventLoopEmscripten wasmEventLoop{};
    EventLoop* eventLoop{getEventLoop()};
    EventLoop* getEventLoop() {
        if (!eventLoop)
            eventLoop = &wasmEventLoop;
        return eventLoop;
    }
#elif (defined(__linux__) || defined(__unix__)) && !defined(__EMSCRIPTEN__)
    // On Linux, use unified EventLoop with X11 + Wayland support (no GTK dependency)
    EventLoopLinux linuxEventLoop{};

    EventLoop* eventLoop{getEventLoop()};
    EventLoop* getEventLoop() {
        if (!eventLoop)
            eventLoop = &linuxEventLoop;
        return eventLoop;
    }
#else
    // On macOS/Windows, use choc by default
    class EventLoopChoc : public EventLoop {
    protected:
        void initializeOnUIThreadImpl() override {
            choc::messageloop::initialise();
        }
        bool runningOnMainThreadImpl() override {
            return choc::messageloop::callerIsOnMessageThread();
        }
        void enqueueTaskOnMainThreadImpl(std::function<void()>&& func) override {
            choc::messageloop::postMessage(std::move(func));
        }
        void startImpl() override {
            choc::messageloop::run();
        }

        void stopImpl() override {
            choc::messageloop::stop();
        }
    };

    EventLoopChoc choc{};

    EventLoop* eventLoop{getEventLoop()};
    EventLoop* getEventLoop() {
        if (!eventLoop)
            eventLoop = &choc;
        return eventLoop;
    }
#endif

    void setEventLoop(EventLoop* newImpl) {
        eventLoop = newImpl;
        assert(eventLoop);
        assert(EventLoop::runningOnMainThread());
    }
}
