#include "PluginFormatCLAP.hpp"

using namespace remidy;

PluginInstanceCLAP::PresetsSupport::PresetsSupport(PluginInstanceCLAP* owner) : owner(owner) {
}

int32_t PluginInstanceCLAP::PresetsSupport::getPresetIndexForId(std::string &id) {
    throw std::runtime_error("FIXME: not implemented");
}

int32_t PluginInstanceCLAP::PresetsSupport::getPresetCount() {
    throw std::runtime_error("FIXME: not implemented");
}

PresetInfo PluginInstanceCLAP::PresetsSupport::getPresetInfo(int32_t index) {
    throw std::runtime_error("FIXME: not implemented");
}

void PluginInstanceCLAP::PresetsSupport::loadPreset(int32_t index) {
    throw std::runtime_error("FIXME: not implemented");
}
