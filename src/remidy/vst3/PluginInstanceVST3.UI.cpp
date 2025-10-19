#include "PluginFormatVST3.hpp"

namespace remidy {

    PluginInstanceVST3::UISupport::UISupport(PluginInstanceVST3* owner) {

    }

    bool PluginInstanceVST3::UISupport::create() {
        return false;
    }

    void PluginInstanceVST3::UISupport::destroy() {

    }

    bool PluginInstanceVST3::UISupport::show() {
        return false;
    }

    void PluginInstanceVST3::UISupport::hide() {

    }

    void PluginInstanceVST3::UISupport::setWindowTitle(std::string title) {

    }

    bool PluginInstanceVST3::UISupport::attachToParent(void *parent) {
        return false;
    }

    bool PluginInstanceVST3::UISupport::canResize() {
        return false;
    }

    bool PluginInstanceVST3::UISupport::getSize(uint32_t &width, uint32_t &height) {
        return false;
    }

    bool PluginInstanceVST3::UISupport::setSize(uint32_t width, uint32_t height) {
        return false;
    }

    bool PluginInstanceVST3::UISupport::suggestSize(uint32_t &width, uint32_t &height) {
        return false;
    }

    bool PluginInstanceVST3::UISupport::setScale(double scale) {
        return false;
    }
}