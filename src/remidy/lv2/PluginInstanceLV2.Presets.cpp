#include "PluginFormatLV2.hpp"

using namespace remidy;

remidy::PluginInstanceLV2::PresetsSupport::PresetsSupport(remidy::PluginInstanceLV2* owner) : owner(owner) {
}

int32_t PluginInstanceLV2::PresetsSupport::getPresetIndexForId(std::string &id) {
    // id = "{index}"
    return std::stoi(id);
}

int32_t PluginInstanceLV2::PresetsSupport::getPresetCount() {
    return items.size();
}

PresetInfo PluginInstanceLV2::PresetsSupport::getPresetInfo(int32_t index) {
    return items[index];
}

void PluginInstanceLV2::PresetsSupport::loadPreset(int32_t index) {
    // FIXME: load preset binary
    throw std::runtime_error("PluginInstanceLV2::PresetsSupport::loadPreset() Not fully implemented");

    std::vector<uint8_t> state{};
    owner->states()->setState(state, PluginStateSupport::StateContextType::Preset, false);
}
