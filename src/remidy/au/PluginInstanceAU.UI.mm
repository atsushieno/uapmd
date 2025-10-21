#include "PluginFormatAU.hpp"

namespace remidy {

    PluginInstanceAU::UISupport::UISupport(PluginInstanceAU* owner) {

    }

    bool PluginInstanceAU::UISupport::create(bool isFloating) {
        (void) isFloating;
        return false;
    }

    void PluginInstanceAU::UISupport::destroy() {

    }

    bool PluginInstanceAU::UISupport::show() {
        return false;
    }

    void PluginInstanceAU::UISupport::hide() {

    }

    void PluginInstanceAU::UISupport::setWindowTitle(std::string title) {

    }

    bool PluginInstanceAU::UISupport::attachToParent(void *parent) {
        return false;
    }

    bool PluginInstanceAU::UISupport::canResize() {
        return false;
    }

    bool PluginInstanceAU::UISupport::getSize(uint32_t &width, uint32_t &height) {
        return false;
    }

    bool PluginInstanceAU::UISupport::setSize(uint32_t width, uint32_t height) {
        return false;
    }

    bool PluginInstanceAU::UISupport::suggestSize(uint32_t &width, uint32_t &height) {
        return false;
    }

    bool PluginInstanceAU::UISupport::setScale(double scale) {
        return false;
    }

    void PluginInstanceAU::UISupport::setResizeRequestHandler(std::function<bool(uint32_t, uint32_t)> handler) {
    }
}
