#include "PluginFormatLV2.hpp"

namespace remidy {

    PluginInstanceLV2::UISupport::UISupport(PluginInstanceLV2* owner) {

    }

    bool PluginInstanceLV2::UISupport::create() {
        return false;
    }

    void PluginInstanceLV2::UISupport::destroy() {

    }

    bool PluginInstanceLV2::UISupport::show() {
        return false;
    }

    void PluginInstanceLV2::UISupport::hide() {

    }

    void PluginInstanceLV2::UISupport::setWindowTitle(std::string title) {

    }

    bool PluginInstanceLV2::UISupport::attachToParent(void *parent) {
        return false;
    }

    bool PluginInstanceLV2::UISupport::canResize() {
        return false;
    }

    bool PluginInstanceLV2::UISupport::getSize(uint32_t &width, uint32_t &height) {
        return false;
    }

    bool PluginInstanceLV2::UISupport::setSize(uint32_t width, uint32_t height) {
        return false;
    }

    bool PluginInstanceLV2::UISupport::suggestSize(uint32_t &width, uint32_t &height) {
        return false;
    }

    bool PluginInstanceLV2::UISupport::setScale(double scale) {
        return false;
    }
}