#if __APPLE__

#include <CoreFoundation/CFRunLoop.h>
#include <remidy.hpp>

void remidy::EventLoop::initializeOnUIThread() {
    // nothing to do here.
}

void remidy::EventLoop::asyncRunOnMainThread(std::function<void()>&& func) {
    // FIXME: we need some cross-platform main thread dispatcher.
    CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopDefaultMode, ^(void) { func(); });
}
#endif
