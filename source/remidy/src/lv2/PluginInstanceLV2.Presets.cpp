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
    if (!state) {
        lilv_node_free(uri);
        return;
    }
    lilv_state_restore(state, owner->instance, nullptr, nullptr, 0, owner->formatImpl->features.data());

    const LilvNode* stateNode = nullptr;
    if (owner->formatImpl->worldContext->state_state_uri_node && uri)
        stateNode = lilv_world_get(owner->formatImpl->world, uri,
                                   owner->formatImpl->worldContext->state_state_uri_node, nullptr);

    auto params = dynamic_cast<PluginInstanceLV2::ParameterSupport*>(owner->parameters());
    if (params) {
        params->refreshAllParameterMetadata();
        auto& paramList = params->parameters();
        auto* unmap = owner->getLV2UridUnmapData();
        auto* lv2UI = dynamic_cast<PluginInstanceLV2::UISupport*>(owner->ui());
        for (size_t i = 0; i < paramList.size(); i++) {
            double value = 0.0;
            bool updatedFromState = false;
            if (stateNode && unmap && unmap->unmap) {
                if (auto propertyUrid = params->propertyUridForIndex(static_cast<uint32_t>(i)); propertyUrid.has_value()) {
                    const char* propertyUri = unmap->unmap(unmap->handle, propertyUrid.value());
                    if (propertyUri) {
                        LilvNode* propertyNode = lilv_new_uri(owner->formatImpl->world, propertyUri);
                        auto values = lilv_world_find_nodes(owner->formatImpl->world, stateNode, propertyNode, nullptr);
                        if (values) {
                            LILV_FOREACH(nodes, iter, values) {
                                auto valueNode = lilv_nodes_get(values, iter);
                                if (!valueNode)
                                    continue;
                                if (lilv_node_is_float(valueNode)) {
                                    value = lilv_node_as_float(valueNode);
                                    updatedFromState = true;
                                    break;
                                } else if (lilv_node_is_int(valueNode)) {
                                    value = lilv_node_as_int(valueNode);
                                    updatedFromState = true;
                                    break;
                                } else if (lilv_node_is_bool(valueNode)) {
                                    value = lilv_node_as_bool(valueNode) ? 1.0 : 0.0;
                                    updatedFromState = true;
                                    break;
                                }
                            }
                            lilv_nodes_free(values);
                        }
                        lilv_node_free(propertyNode);
                    }
                    if (updatedFromState) {
                        params->updateCachedParameterValue(static_cast<uint32_t>(i), value);
                        params->notifyParameterValue(static_cast<uint32_t>(i), value);
                        if (lv2UI)
                            lv2UI->notifyParameterChange(propertyUrid.value(), value);
                        continue;
                    }
                }
            }

            if (params->getParameter(static_cast<uint32_t>(i), &value) == StatusCode::OK) {
                params->notifyParameterValue(static_cast<uint32_t>(i), value);
                if (lv2UI) {
                    if (auto propertyUrid = params->propertyUridForIndex(static_cast<uint32_t>(i)); propertyUrid.has_value())
                        lv2UI->notifyParameterChange(propertyUrid.value(), value);
                }
            }
        }
    }

    lilv_state_free(state);
    lilv_node_free(uri);
}
