#pragma once

#include <functional>

namespace remidy {

    class EventLoop {
    public:
        static void initializeOnUIThread();
        static bool runningOnMainThread();
        // Run task either immediately (if current thread is the main thread) or asynchronously (otherwise).
        static void runTaskOnMainThread(std::function<void()> func);
        // Enqueue task on the main thread to run asynchronously.
        static void enqueueTaskOnMainThread(std::function<void()> func);
        static void start();
        static void stop();
    };

}