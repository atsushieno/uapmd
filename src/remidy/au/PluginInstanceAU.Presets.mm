#if __APPLE__

#include "PluginFormatAU.hpp"

#include "PluginFormatAU.hpp"

using namespace remidy;

AudioPluginInstanceAU::PresetsSupport::PresetsSupport(AudioPluginInstanceAU* owner) : owner(owner) {
}

int32_t AudioPluginInstanceAU::PresetsSupport::getPresetIndexForId(std::string &id) {
    throw std::runtime_error("FIXME: not implemented");
}

int32_t AudioPluginInstanceAU::PresetsSupport::getPresetCount() {
    std::cerr << "FIXME: PresetsSupport::getPresetCount not implemented" << std::endl;
    return 0;
}

PresetInfo AudioPluginInstanceAU::PresetsSupport::getPresetInfo(int32_t index) {
    throw std::runtime_error("FIXME: not implemented");
}

void AudioPluginInstanceAU::PresetsSupport::loadPreset(int32_t index) {
    throw std::runtime_error("FIXME: not implemented");
}

#endif
