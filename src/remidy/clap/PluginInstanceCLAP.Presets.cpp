#include "PluginFormatCLAP.hpp"

using namespace remidy;

PluginInstanceCLAP::PresetsSupport::PresetsSupport(PluginInstanceCLAP* owner) : owner(owner) {
    presets = remidy_clap::PresetLoader::loadPresets(owner->preset_discovery_factory);
}

PluginInstanceCLAP::PresetsSupport::~PresetsSupport() {
}

int32_t PluginInstanceCLAP::PresetsSupport::getPresetIndexForId(std::string &id) {
    for (int32_t i = 0, n = (int32_t) presets.size(); i < n; i++)
        if (presets[i].name == id)
            return i;
    return -1;
}

int32_t PluginInstanceCLAP::PresetsSupport::getPresetCount() {
    return (int32_t) presets.size();
}

PresetInfo PluginInstanceCLAP::PresetsSupport::getPresetInfo(int32_t index) {
    auto& p = presets[index];
    return PresetInfo { p.name, p.name, 0, index };
}

void PluginInstanceCLAP::PresetsSupport::loadPreset(int32_t index) {
    if (!owner->plugin || !owner->plugin->canUsePresetLoad())
        return;

    EventLoop::runTaskOnMainThread([&] {
        // So, how do you specify the preset at index when the specification says "for factory presets, location must be NULL" ? load_key is just a category like "Bass"
        if (!owner->plugin->presetLoadFromLocation(CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, presets[index].load_key.c_str()))
            Logger::global()->logWarning("Failed to load preset: %s", presets[index].name.c_str());
    });
}
