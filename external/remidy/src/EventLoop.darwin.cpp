#if __APPLE__

#include <dispatch/dispatch.h>
#include <CoreFoundation/CFRunLoop.h>
#include <remidy.hpp>

void remidy::EventLoop::initializeOnUIThread() {
    // nothing to do here.
}

void remidy::EventLoop::asyncRunOnMainThread(std::function<void()> func) {
    auto cur = CFRunLoopGetCurrent();
    auto main = CFRunLoopGetMain();
    if (cur == main)
        func();
    else
        dispatch_async(dispatch_get_main_queue(),  ^(void) {
            func();
        });
}

void remidy::EventLoop::start() {
}
#endif
