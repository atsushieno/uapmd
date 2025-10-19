#include "PluginFormatCLAP.hpp"

namespace remidy {

    PluginInstanceCLAP::UISupport::UISupport(PluginInstanceCLAP* owner) {

    }

    bool PluginInstanceCLAP::UISupport::create() {
        return false;
    }

    void PluginInstanceCLAP::UISupport::destroy() {

    }

    bool PluginInstanceCLAP::UISupport::show() {
        return false;
    }

    void PluginInstanceCLAP::UISupport::hide() {

    }

    void PluginInstanceCLAP::UISupport::setWindowTitle(std::string title) {

    }

    bool PluginInstanceCLAP::UISupport::attachToParent(void *parent) {
        return false;
    }

    bool PluginInstanceCLAP::UISupport::canResize() {
        return false;
    }

    bool PluginInstanceCLAP::UISupport::getSize(uint32_t &width, uint32_t &height) {
        return false;
    }

    bool PluginInstanceCLAP::UISupport::setSize(uint32_t width, uint32_t height) {
        return false;
    }

    bool PluginInstanceCLAP::UISupport::suggestSize(uint32_t &width, uint32_t &height) {
        return false;
    }

    bool PluginInstanceCLAP::UISupport::setScale(double scale) {
        return false;
    }
}