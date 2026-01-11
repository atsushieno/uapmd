#include <remidy/priv/event-loop.hpp>

namespace remidy {

namespace {
class SimpleEventLoop : public EventLoop {
protected:
    void initializeOnUIThreadImpl() override {}
    bool runningOnMainThreadImpl() override { return true; }
    void enqueueTaskOnMainThreadImpl(std::function<void()>&& func) override { if (func) func(); }
    void startImpl() override {}
    void stopImpl() override {}
};

SimpleEventLoop defaultLoop{};
EventLoop* g_loop = &defaultLoop;
}

EventLoop* getEventLoop() { return g_loop; }
void setEventLoop(EventLoop* eventLoop) { g_loop = eventLoop ? eventLoop : &defaultLoop; }

} // namespace remidy

