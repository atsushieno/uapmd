#include "PluginFormatCLAP.hpp"

using namespace remidy;

PluginInstanceCLAP::PresetsSupport::PresetsSupport(PluginInstanceCLAP* owner) : owner(owner) {
    auto factory = owner->preset_discovery_factory;
    if (factory) {
        for (int32_t i = 0, n = (int32_t) factory->count(factory); i < n; i++) {
            auto desc = factory->get_descriptor(factory, i);
            items.emplace_back(desc->id, desc->name, 0, i);
        }
    }
}

int32_t PluginInstanceCLAP::PresetsSupport::getPresetIndexForId(std::string &id) {
    for (int32_t i = 0, n = (int32_t) items.size(); i < n; i++)
        if (items[i].id() == id)
            return i;
    return -1;
}

int32_t PluginInstanceCLAP::PresetsSupport::getPresetCount() {
    return (int32_t) items.size();
}

PresetInfo PluginInstanceCLAP::PresetsSupport::getPresetInfo(int32_t index) {
    return items[index];
}

void PluginInstanceCLAP::PresetsSupport::loadPreset(int32_t index) {
    auto preset_load_ext = (clap_plugin_preset_load*) owner->plugin->get_extension(owner->plugin, CLAP_EXT_PRESET_LOAD);
    if (preset_load_ext)
        preset_load_ext->from_location(owner->plugin, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, items[index].id().c_str());
}
