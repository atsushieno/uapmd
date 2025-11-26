#include "PluginFormatLV2.hpp"

using namespace remidy;

remidy::PluginInstanceLV2::PresetsSupport::PresetsSupport(remidy::PluginInstanceLV2* owner) : owner(owner) {

    preset_nodes = lilv_plugin_get_related(owner->plugin, owner->formatImpl->worldContext->presets_preset_node);
    LILV_FOREACH(nodes, i, preset_nodes) {
        auto preset_node = lilv_nodes_get(preset_nodes, i);
        auto label_node = lilv_world_get(owner->formatImpl->world, preset_node, owner->formatImpl->worldContext->rdfs_label_node, nullptr);
        items.emplace_back(lilv_node_as_uri(preset_node), lilv_node_as_string(label_node), 0, (int32_t) items.size());
    }
}

int32_t PluginInstanceLV2::PresetsSupport::getPresetIndexForId(std::string &id) {
    for (size_t i = 0, n = items.size(); i < n; i++)
        if (items[i].id() == id)
            return (int32_t) i;
    return -1;
}

int32_t PluginInstanceLV2::PresetsSupport::getPresetCount() {
    return items.size();
}

PresetInfo PluginInstanceLV2::PresetsSupport::getPresetInfo(int32_t index) {
    return items[index];
}

void PluginInstanceLV2::PresetsSupport::loadPreset(int32_t index) {
    auto uri = lilv_new_uri(owner->formatImpl->world, items[index].id().c_str());

    auto state = lilv_state_new_from_world(owner->formatImpl->world,
                                           &owner->formatImpl->worldContext->features.urid_map_feature_data,
                                           uri);
    lilv_state_restore(state, owner->instance, nullptr, nullptr, 0, owner->formatImpl->features.data());

    // Refresh parameter metadata and poll values after preset load
    // This handles plugins that may change parameter ranges or not emit proper change notifications
    auto params = dynamic_cast<PluginInstanceLV2::ParameterSupport*>(owner->parameters());
    if (params) {
        params->refreshAllParameterMetadata();
        auto& paramList = params->parameters();
        for (size_t i = 0; i < paramList.size(); i++) {
            double value;
            if (params->getParameter(static_cast<uint32_t>(i), &value) == StatusCode::OK)
                params->notifyParameterValue(static_cast<uint32_t>(i), value);
        }
    }
}
