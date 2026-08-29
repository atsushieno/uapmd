#include <assert.h>
#include <iostream>
#include <queue>
#include <mutex>
#if ANDROID
#include <android/looper.h>
#elif defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#include <emscripten/threading.h>
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
            // Answering "yes" unconditionally is not harmless here. A pthread is a
            // Web Worker with its own JS scope: no document, no AudioWorklet node,
            // and its own copy of Module. Claiming a worker is the main thread makes
            // runTaskOnMainThread() run the task *there*, so anything that touches
            // the DOM or the worklet bridge quietly builds a second, unreachable one
            // and waits forever for a reply that can never arrive. Plug-in scanning
            // runs on a std::thread, which is exactly that case.
            return emscripten_is_main_browser_thread();
        }

        void enqueueTaskOnMainThreadImpl(std::function<void()>&& func) override {
            {
                std::lock_guard<std::mutex> lock(queueMutex_);
                taskQueue_.emplace(std::move(func));
            }
            if (emscripten_is_main_browser_thread()) {
                emscripten_async_call(&EventLoopEmscripten::drainTasksThunk, this, 0);
                return;
            }
            // From a worker the drain has to be proxied, or it would run back on the
            // worker and defeat the point of enqueueing it.
            emscripten_async_run_in_main_runtime_thread(
                EM_FUNC_SIG_VI, reinterpret_cast<void*>(&EventLoopEmscripten::drainTasksThunk), this);
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
