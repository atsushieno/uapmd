#include "PluginFormatCLAP.hpp"
#include <clap/ext/audio-ports.h>

namespace remidy {
    void PluginInstanceCLAP::AudioBuses::inspectBuses() {
        input_bus_defs.clear();
        output_bus_defs.clear();
        for (auto bus: audio_in_buses)
            delete bus;
        for (auto bus: audio_out_buses)
            delete bus;

        BusSearchResult ret{};

        auto plugin = owner->plugin;
        auto ext = (clap_plugin_audio_ports_t*) plugin->get_extension(plugin, CLAP_EXT_AUDIO_PORTS);
        if (!ext)
            return;
        for (bool isInput : {true, false}) {
            for (size_t i = 0, n = ext->count(plugin, isInput); i < n; i++) {
                clap_audio_port_info_t info;
                if (!ext->get(plugin, i, isInput, &info))
                    continue;
                std::vector<AudioChannelLayout> layouts{};
                AudioChannelLayout layout{info.port_type == CLAP_PORT_MONO ? "Mono" : CLAP_PORT_STEREO ? "Stereo" : "", info.channel_count};
                layouts.emplace_back(layout);
                AudioBusDefinition def{info.name, info.flags & CLAP_AUDIO_PORT_IS_MAIN ? AudioBusRole::Main : AudioBusRole::Aux, layouts};
                if (isInput) {
                    ret.numAudioIn++;
                    input_bus_defs.emplace_back(def);
                } else {
                    ret.numAudioOut++;
                    output_bus_defs.emplace_back(def);
                }
            }
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
    }
}
