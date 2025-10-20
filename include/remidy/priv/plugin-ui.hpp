#pragma once

#include <functional>
#include <string>

namespace remidy {
    class PluginUISupport {
    public:
        PluginUISupport() = default;
        virtual ~PluginUISupport() = default;

        virtual bool create(bool isFloating) = 0;
        virtual void destroy() = 0;

        virtual bool show() = 0;
        virtual void hide() = 0;

        virtual void setWindowTitle(std::string title) = 0;

        virtual bool attachToParent(void* parent) = 0;

        virtual bool canResize() = 0;
        virtual bool getSize(uint32_t &width, uint32_t &height) = 0;
        virtual bool setSize(uint32_t width, uint32_t height) = 0;
        virtual bool suggestSize(uint32_t &width, uint32_t &height) = 0;

        virtual bool setScale(double scale) = 0;

        // interaction from the plugin side.
        virtual void setResizeRequestHandler(std::function<bool(uint32_t, uint32_t)> handler) = 0;
    };
}
