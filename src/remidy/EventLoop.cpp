#include <assert.h>
#include "remidy.hpp"
#if !ANDROID
#include <choc/gui/choc_MessageLoop.h>
#endif

#if (defined(__linux__) || defined(__unix__)) && !ANDROID && !EMSCRIPTEN
#include "EventLoopLinux.hpp"
#endif

namespace remidy {
#if ANDROID || EMSCRIPTEN
    EventLoop* eventLoop{getEventLoop()};
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