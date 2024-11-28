#pragma once

#include <remidy/remidy.hpp>
//#include <nui/frontend.hpp>
#include <nui/core.hpp>
#include <nui/window.hpp>
//#include <nui/rpc_hub.hpp>

namespace uapmd {
    class EventLoopWebViewH : public remidy::EventLoop {
        Nui::Window window;

    protected:
        void initializeOnUIThreadImpl() override {
        }

        bool runningOnMainThreadImpl() override {
            return false; // We cannot really know this on Nui, so fallback to safer option.
        }

        void enqueueTaskOnMainThreadImpl(std::function<void()> &&func) override {
            window.dispatch(std::move(func));
        }

        void startImpl() override {
            window.run();
        }

        void stopImpl() override {
            window.terminate();
        }

    };
}