#include "PluginFormatLV2.hpp"

void remidy::PluginInstanceLV2::AudioBuses::inspectBuses() {
    auto plugin = owner->plugin;
    auto& implContext = owner->implContext;

    BusSearchResult ret{};

    input_bus_defs.clear();
    output_bus_defs.clear();
    input_buses.clear();
    output_buses.clear();
    for (uint32_t p = 0; p < lilv_plugin_get_num_ports(plugin); p++) {
        auto port = lilv_plugin_get_port_by_index(plugin, p);
        if (implContext.IS_AUDIO_PORT(plugin, port)) {
            bool isInput = implContext.IS_INPUT_PORT(plugin, port);
            auto groupNode = lilv_port_get(plugin, port, implContext.statics->port_group_uri_node);
            std::string group = groupNode == nullptr ? "" : lilv_node_as_string(groupNode);
            auto scNode = lilv_port_get(plugin, port, implContext.statics->is_side_chain_uri_node);
            bool isSideChain = scNode != nullptr && lilv_node_as_bool(scNode);
            std::optional<AudioBusDefinition> def{};
            int32_t index = 0;
            for (auto d : isInput ? input_bus_defs : output_bus_defs) {
                if (d.name() == group) {
                    def = d;
                    break;
                }
                index++;
            }
            if (!def.has_value()) {
                def = AudioBusDefinition(group, isSideChain ? AudioBusRole::Aux : AudioBusRole::Main);
                (isInput ? input_bus_defs : output_bus_defs).emplace_back(def.value());
                auto bus = new AudioBusConfiguration(def.value());
                bus->channelLayout(AudioChannelLayout::mono());
                (isInput ? input_buses : output_buses).emplace_back(bus);
            } else {
                auto bus = (isInput ? input_buses : output_buses)[index];
                if (bus->channelLayout() != AudioChannelLayout::mono())
                    bus->channelLayout(AudioChannelLayout::stereo());
                else
                    throw std::runtime_error{"Audio ports more than stereo channels are not supported yet."};
            }
        }
        if (implContext.IS_ATOM_IN(plugin, port))
            ret.numEventIn++;
        if (implContext.IS_ATOM_OUT(plugin, port))
            ret.numEventOut++;
    }
    busesInfo = ret;
}

