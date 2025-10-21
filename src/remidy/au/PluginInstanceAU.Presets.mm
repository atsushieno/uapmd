#if __APPLE__

#include "PluginFormatAU.hpp"

#include "PluginFormatAU.hpp"

using namespace remidy;

PluginInstanceAU::PresetsSupport::PresetsSupport(PluginInstanceAU* owner) : owner(owner) {
}

int32_t PluginInstanceAU::PresetsSupport::getPresetIndexForId(std::string &id) {
    throw std::runtime_error("FIXME: not implemented");
}

int32_t PluginInstanceAU::PresetsSupport::getPresetCount() {
    std::cerr << "FIXME: PresetsSupport::getPresetCount not implemented" << std::endl;
    return 0;
}

PresetInfo PluginInstanceAU::PresetsSupport::getPresetInfo(int32_t index) {
    throw std::runtime_error("FIXME: not implemented");
}

void PluginInstanceAU::PresetsSupport::loadPreset(int32_t index) {
    throw std::runtime_error("FIXME: not implemented");
}

#endif
