#include "remidy.hpp"
#include <choc/gui/choc_MessageLoop.h>

// FIXME: maybe we will not use choc on Linux as it depends on Gtk which is in principle unacceptable.
namespace remidy {
    void EventLoop::initializeOnUIThread() {
        choc::messageloop::initialise();
    }
    bool EventLoop::runningOnMainThread() {
        return choc::messageloop::callerIsOnMessageThread();
    }

    void EventLoop::runTaskOnMainThread(std::function<void()> func) {
        if (choc::messageloop::callerIsOnMessageThread())
            func();
        else
            choc::messageloop::postMessage(std::move(func));
    }
    void EventLoop::enqueueTaskOnMainThread(std::function<void()> func) {
        choc::messageloop::postMessage(std::move(func));
    }
    void EventLoop::start() {
        choc::messageloop::run();
    }

    void EventLoop::stop() {
        choc::messageloop::stop();
    }
}