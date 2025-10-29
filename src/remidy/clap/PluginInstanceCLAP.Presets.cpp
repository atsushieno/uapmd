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
    clap_plugin_preset_load_t* preset_load_ext{nullptr};
    EventLoop::runTaskOnMainThread([&] {
        preset_load_ext = (clap_plugin_preset_load*) owner->plugin->get_extension(owner->plugin, CLAP_EXT_PRESET_LOAD);
    });
    if (preset_load_ext)
        // So, how do you specify the preset at index when the specification says "for factory presets, location must be NULL" ? load_key is just a category like "Bass"
        if (!preset_load_ext->from_location(owner->plugin, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, presets[index].load_key.c_str()))
            Logger::global()->logWarning("Failed to load preset: %s", presets[index].name.c_str());
}
