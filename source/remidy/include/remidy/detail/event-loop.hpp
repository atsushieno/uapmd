#pragma once

#include <functional>
#include <atomic>

namespace remidy {
    class EventLoop;

    EventLoop* getEventLoop();
    void setEventLoop(EventLoop* eventLoop);

    // Provides the way for plugin UI support implementation to get access to UI event loop
    // so that it can appropriately dispatch some function invocation on the UI thread.
    //
    // Call `initializeOnUIThread()` once, and `start()` to run the main UI message loop.
    // It will then go into infinite message loop until `stop()` is invoked by some event
    // (or externally invoked on another thread).
    //
    // It is designed to be "pluggable" so that the actual hosting application can work with
    // any UI framework that offers way to initialize event loop and dispatch UI events.
    // Implement (derive from) this class and call `instance(EventLoop)` to use it.
    // Make sure to do this before invoking `initializeOnUIThread()` (or any other functions).
    //
    // The default implementation (exists) is based on choc (`choc::gui::MessageLoop`).
    class EventLoop {
    protected:
        virtual void initializeOnUIThreadImpl() = 0;
        virtual bool runningOnMainThreadImpl() = 0;
        virtual void enqueueTaskOnMainThreadImpl(std::function<void()>&& func) = 0;
        virtual void startImpl() = 0;
        virtual void stopImpl() = 0;
    public:
        virtual ~EventLoop() = default;

        static void initializeOnUIThread() { getEventLoop()->initializeOnUIThreadImpl(); }
        static bool runningOnMainThread() { return getEventLoop()->runningOnMainThreadImpl(); }
        // Run task either immediately (if current thread is the main thread) or asynchronously (otherwise).
        static void runTaskOnMainThread(std::function<void()>&& func) {
            if (runningOnMainThread())
                func();
            else {
                std::atomic<bool> done(false);
                enqueueTaskOnMainThread([&done,func]() {
                    func();
                    done.store(true);
                    done.notify_one();
                });
                done.wait(false);
            }
        }
        // Enqueue task on the main thread to run asynchronously.
        static void enqueueTaskOnMainThread(std::function<void()>&& func) { getEventLoop()->enqueueTaskOnMainThreadImpl(std::move(func)); }
        static void start() { getEventLoop()->startImpl(); }
        static void stop() { getEventLoop()->stopImpl(); }
    };

}