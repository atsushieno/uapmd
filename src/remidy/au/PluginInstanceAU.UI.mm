#include "PluginFormatAU.hpp"

namespace remidy {

    AudioPluginInstanceAU::UISupport::UISupport(AudioPluginInstanceAU* owner) {

    }

    bool AudioPluginInstanceAU::UISupport::create() {
        return false;
    }

    void AudioPluginInstanceAU::UISupport::destroy() {

    }

    bool AudioPluginInstanceAU::UISupport::show() {
        return false;
    }

    void AudioPluginInstanceAU::UISupport::hide() {

    }

    void AudioPluginInstanceAU::UISupport::setWindowTitle(std::string title) {

    }

    bool AudioPluginInstanceAU::UISupport::attachToParent(void *parent) {
        return false;
    }

    bool AudioPluginInstanceAU::UISupport::canResize() {
        return false;
    }

    bool AudioPluginInstanceAU::UISupport::getSize(uint32_t &width, uint32_t &height) {
        return false;
    }

    bool AudioPluginInstanceAU::UISupport::setSize(uint32_t width, uint32_t height) {
        return false;
    }

    bool AudioPluginInstanceAU::UISupport::suggestSize(uint32_t &width, uint32_t &height) {
        return false;
    }

    bool AudioPluginInstanceAU::UISupport::setScale(double scale) {
        return false;
    }
}