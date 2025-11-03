#include "PluginFormatLV2.hpp"
#include <optional>
#include <algorithm>
#include <vector>

void remidy::PluginInstanceLV2::AudioBuses::inspectBuses() {
    auto plugin = owner->plugin;
    auto& implContext = owner->implContext;

    BusSearchResult ret{};

    input_bus_defs.clear();
    output_bus_defs.clear();
    audio_in_buses.clear();
    audio_out_buses.clear();
    for (uint32_t p = 0, n = lilv_plugin_get_num_ports(plugin); p < n; p++) {
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
                (isInput ? audio_in_buses : audio_out_buses).emplace_back(bus);
                if (isInput)
                    ret.numAudioIn++;
                else
                    ret.numAudioOut++;
            } else {
                auto bus = (isInput ? audio_in_buses : audio_out_buses)[index];
                auto currentChannels = bus->channelLayout().channels();
                auto nextChannels = currentChannels + 1;
                if (nextChannels > 2)
                    throw std::runtime_error{"Audio ports more than stereo channels are not supported yet."};
                if (nextChannels == 1)
                    bus->channelLayout(AudioChannelLayout::mono());
                else if (nextChannels == 2)
                    bus->channelLayout(AudioChannelLayout::stereo());
                else
                    bus->channelLayout(AudioChannelLayout{"", nextChannels});
            }
        }
        if (implContext.IS_ATOM_IN(plugin, port))
            ret.numEventIn++;
        if (implContext.IS_ATOM_OUT(plugin, port))
            ret.numEventOut++;
    }
    busesInfo = ret;
}

void remidy::PluginInstanceLV2::AudioBuses::configure(remidy::PluginInstance::ConfigurationRequest &config) {
    auto applyRequestedChannels = [](std::vector<AudioBusConfiguration*>& buses, int32_t busIndex, const std::optional<uint32_t>& requested) {
        if (!requested.has_value())
            return;
        if (busIndex < 0 || static_cast<size_t>(busIndex) >= buses.size())
            return;
        auto bus = buses[static_cast<size_t>(busIndex)];
        auto channels = requested.value();
        bus->enabled(channels > 0);
    };

    applyRequestedChannels(audio_in_buses, mainInputBusIndex(), config.mainInputChannels);
    applyRequestedChannels(audio_out_buses, mainOutputBusIndex(), config.mainOutputChannels);
}
