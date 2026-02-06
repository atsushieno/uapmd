#include <assert.h>
#include <iostream>
#include "remidy.hpp"
#if ANDROID
#include <android/looper.h>
#elif (defined(__linux__) || defined(__unix__)) && !ANDROID && !EMSCRIPTEN
#include "EventLoopLinux.hpp"
#else
#include <choc/gui/choc_MessageLoop.h>
#endif

namespace remidy {
#if ANDROID || EMSCRIPTEN
    EventLoop* eventLoop{getEventLoop()};

    class EventLoopAndroid : public EventLoop {
        ALooper* looper;

        protected:
        void initializeOnUIThreadImpl() override {
            ALooper_acquire(looper);
        }
        bool runningOnMainThreadImpl() override {
            return ALooper_forThread() == looper;
        }
        void enqueueTaskOnMainThreadImpl(std::function<void()>&& func) override {
            std::cerr << "Android does not support enqueueing tasks on the main thread" << std::endl;
            func();
        }
        void startImpl() override {
            // FIXME: what can we do here?
        }

        void stopImpl() override {
            // FIXME: what can we do here?
        }
    };
    EventLoopAndroid androidEventLoop{};
    EventLoop* getEventLoop() {
        if (!eventLoop)
            eventLoop = &androidEventLoop;
        return eventLoop;
    }
#elif (defined(__linux__) || defined(__unix__))
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