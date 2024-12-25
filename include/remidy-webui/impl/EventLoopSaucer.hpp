#include "saucer/smartview.hpp"
#include "remidy/remidy.hpp"

namespace remidy::webui::saucer_wrapper {

class EventLoopSaucer : public remidy::EventLoop {
    std::shared_ptr<saucer::application> app;

protected:
    void initializeOnUIThreadImpl() override {
        // since the constructor takes application, it should have already been initialized at caller.
    }

    bool runningOnMainThreadImpl() override {
        return app->thread_safe();
    }

    void enqueueTaskOnMainThreadImpl(std::function<void()> &&func) override {
        app->post(std::move_only_function<void()>{ func });
    }

    void startImpl() override {
        app->run();
    }

    void stopImpl() override {
        app->quit();
    }

public:
    EventLoopSaucer(std::shared_ptr<saucer::application> app) : app(app) {}
};

}
