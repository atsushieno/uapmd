#include "remidy.hpp"
#include <choc/gui/choc_MessageLoop.h>

// FIXME: maybe we will not use choc on Linux as it depends on Gtk which is in principle unacceptable.
namespace remidy {
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
    EventLoop* impl{&choc};
    EventLoop* EventLoop::instance() {
        return impl;
    }
    void EventLoop::instance(EventLoop& newImpl) {
        impl = &newImpl;
    }
}