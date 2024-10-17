#pragma once

#include <functional>

namespace remidy {

    class EventLoop {
    public:
        static void initializeOnUIThread();
        static void asyncRunOnMainThread(std::function<void()>&& func);
    };

}