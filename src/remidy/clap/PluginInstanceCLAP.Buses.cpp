#include "PluginFormatCLAP.hpp"
#include <clap/ext/audio-ports.h>
#include <clap/ext/note-ports.h>
#include <optional>
#include <map>

namespace remidy {
    void PluginInstanceCLAP::AudioBuses::inspectBuses() {
        input_bus_defs.clear();
        output_bus_defs.clear();
        for (auto bus: audio_in_buses)
            delete bus;
        for (auto bus: audio_out_buses)
            delete bus;

        BusSearchResult ret{};

        auto plugin = owner->plugin.get();
        if (plugin && plugin->canUseAudioPorts()) {
            // First pass: collect all port info
            std::map<clap_id, size_t> inputPortIdToIndex;
            std::map<clap_id, size_t> outputPortIdToIndex;
            std::vector<clap_audio_port_info_t> inputPortInfos;
            std::vector<clap_audio_port_info_t> outputPortInfos;

            for (bool isInput : {true, false}) {
                auto& portInfos = isInput ? inputPortInfos : outputPortInfos;
                auto& portIdMap = isInput ? inputPortIdToIndex : outputPortIdToIndex;

                for (size_t i = 0, n = plugin->audioPortsCount(isInput); i < n; i++) {
                    clap_audio_port_info_t info;
                    if (!plugin->audioPortsGet(i, isInput, &info))
                        continue;
                    portInfos.push_back(info);
                    portIdMap[info.id] = i;
                }
            }

            // Store port infos for later use (in-place processing detection)
            owner->inputPortInfos = inputPortInfos;
            owner->outputPortInfos = outputPortInfos;

            // Second pass: build bus definitions
            for (size_t i = 0; i < inputPortInfos.size(); i++) {
                const auto& info = inputPortInfos[i];
                std::vector<AudioChannelLayout> layouts{};
                AudioChannelLayout layout{info.port_type == CLAP_PORT_MONO ? "Mono" : info.port_type == CLAP_PORT_STEREO ? "Stereo" : "", info.channel_count};
                layouts.emplace_back(layout);
                AudioBusDefinition def{info.name, info.flags & CLAP_AUDIO_PORT_IS_MAIN ? AudioBusRole::Main : AudioBusRole::Aux, layouts};
                ret.numAudioIn++;
                input_bus_defs.emplace_back(def);
            }

            for (size_t i = 0; i < outputPortInfos.size(); i++) {
                const auto& info = outputPortInfos[i];
                std::vector<AudioChannelLayout> layouts{};
                AudioChannelLayout layout{info.port_type == CLAP_PORT_MONO ? "Mono" : info.port_type == CLAP_PORT_STEREO ? "Stereo" : "", info.channel_count};
                layouts.emplace_back(layout);
                AudioBusDefinition def{info.name, info.flags & CLAP_AUDIO_PORT_IS_MAIN ? AudioBusRole::Main : AudioBusRole::Aux, layouts};
                ret.numAudioOut++;
                output_bus_defs.emplace_back(def);
            }
        }

        // FIXME: we need decent support for event buses
        if (plugin && plugin->canUseNotePorts()) {
            ret.numEventIn = plugin->notePortsCount(true);
            ret.numEventOut = plugin->notePortsCount(false);
        }
        busesInfo = ret;
    }

    void PluginInstanceCLAP::AudioBuses::configure(ConfigurationRequest& configuration) {
        // there should be some audio ports configuration in the future. For now, we adjust bus configs from defs.
        for (auto bus: audio_in_buses)
            delete bus;
        for (auto bus: audio_out_buses)
            delete bus;
        audio_in_buses.clear();
        audio_out_buses.clear();

        for (auto bus: input_bus_defs)
            audio_in_buses.emplace_back(new AudioBusConfiguration(bus));
        for (auto bus: output_bus_defs)
            audio_out_buses.emplace_back(new AudioBusConfiguration(bus));

        auto applyRequestedChannels = [](std::vector<AudioBusConfiguration*>& buses, int32_t busIndex, const std::optional<uint32_t>& requested) {
            if (!requested.has_value())
                return;
            if (busIndex < 0 || static_cast<size_t>(busIndex) >= buses.size())
                return;
            auto bus = buses[static_cast<size_t>(busIndex)];
            uint32_t channels = requested.value();
            bus->enabled(channels > 0);
            if (channels == 0)
                return;
            remidy::AudioChannelLayout layout{"", channels};
            if (channels == 1)
                layout = remidy::AudioChannelLayout{"Mono", 1};
            else if (channels == 2)
                layout = remidy::AudioChannelLayout{"Stereo", 2};
            if (bus->channelLayout(layout) != remidy::StatusCode::OK)
                bus->channelLayout() = layout;
        };

        applyRequestedChannels(audio_in_buses, mainInputBusIndex(), configuration.mainInputChannels);
        applyRequestedChannels(audio_out_buses, mainOutputBusIndex(), configuration.mainOutputChannels);
    }
}
