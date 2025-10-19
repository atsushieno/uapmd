#include "PluginFormatCLAP.hpp"

using namespace remidy;

PluginInstanceCLAP::PresetsSupport::PresetsSupport(PluginInstanceCLAP* owner) : owner(owner) {
    auto factory = owner->preset_discovery_factory;
    if (!factory)
        return;
    clap_preset_discovery_indexer_t indexer {
            .clap_version = CLAP_VERSION,
            .name = "Remidy",
            .vendor = "UAPMD Project",
            .url = "https://github.com/atsushieno/uapmd",
            .version = "0.0.1",
            .indexer_data = this,
            .declare_filetype = declare_filetype,
            .declare_location = declare_location,
            .declare_soundpack = declare_soundpack,
            .get_extension = get_extension
    };
    clap_preset_discovery_metadata_receiver_t receiver {
            .receiver_data = this,
            .on_error = on_error,
            .begin_preset = begin_preset,
            .add_plugin_id = add_plugin_id,
            .set_soundpack_id = set_soundpack_id,
            .set_flags = set_flags,
            .add_creator = add_creator,
            .set_description = set_description,
            .set_timestamps = set_timestamps,
            .add_feature = add_feature,
            .add_extra_info = add_extra_info,
    };

    for (int32_t i = 0, n = (int32_t) factory->count(factory); i < n; i++) {
        auto desc = factory->get_descriptor(factory, i);
        if (!clap_version_is_compatible(desc->clap_version))
            continue;
        auto provider = factory->create(factory, &indexer, desc->id);
        providers.emplace_back(provider);
        provider->init(provider);

        if (!provider->get_metadata(provider, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, &receiver))
            Logger::global()->logWarning("Failed to get preset metadata from provider: %s", desc->name);
    }
}

PluginInstanceCLAP::PresetsSupport::~PresetsSupport() {
    for (auto provider : providers)
        provider->destroy(provider);
    providers.clear();
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
    auto preset_load_ext = (clap_plugin_preset_load*) owner->plugin->get_extension(owner->plugin, CLAP_EXT_PRESET_LOAD);
    if (preset_load_ext)
        if (!preset_load_ext->from_location(owner->plugin, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, presets[index].name.c_str(), presets[index].load_key.c_str()))
            Logger::global()->logWarning("Failed to load preset: %s", presets[index].name.c_str());
}
