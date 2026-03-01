#include "PluginFormatCLAP.hpp"
#include <clap/ext/audio-ports.h>
#include <clap/ext/audio-ports-config.h>
#include <clap/ext/surround.h>
#include <clap/ext/note-ports.h>
#include <optional>

namespace {
    // Helper to get layout name from CLAP surround channel map
    std::string getLayoutNameFromChannelMap(const std::vector<uint8_t>& channelMap) {
        if (channelMap.empty())
            return "";

        uint32_t channels = static_cast<uint32_t>(channelMap.size());

        // Count channel types
        int fl = 0, fr = 0, fc = 0, lfe = 0, bl = 0, br = 0, sl = 0, sr = 0, bc = 0;
        int tfl = 0, tfc = 0, tfr = 0, tbl = 0, tbc = 0, tbr = 0;

        for (auto ch : channelMap) {
            switch (ch) {
                case CLAP_SURROUND_FL: fl++; break;
                case CLAP_SURROUND_FR: fr++; break;
                case CLAP_SURROUND_FC: fc++; break;
                case CLAP_SURROUND_LFE: lfe++; break;
                case CLAP_SURROUND_BL: bl++; break;
                case CLAP_SURROUND_BR: br++; break;
                case CLAP_SURROUND_SL: sl++; break;
                case CLAP_SURROUND_SR: sr++; break;
                case CLAP_SURROUND_BC: bc++; break;
                case CLAP_SURROUND_TFL: tfl++; break;
                case CLAP_SURROUND_TFC: tfc++; break;
                case CLAP_SURROUND_TFR: tfr++; break;
                case CLAP_SURROUND_TBL: tbl++; break;
                case CLAP_SURROUND_TBC: tbc++; break;
                case CLAP_SURROUND_TBR: tbr++; break;
            }
        }

        // Identify common configurations
        if (channels == 1 && (fl == 1 || fc == 1))
            return "Mono";
        else if (channels == 2 && fl == 1 && fr == 1)
            return "Stereo";
        else if (channels == 3 && fl == 1 && fr == 1 && fc == 1)
            return "3.0";
        else if (channels == 4 && fl == 1 && fr == 1 && bl == 1 && br == 1)
            return "Quadraphonic";
        else if (channels == 5 && fl == 1 && fr == 1 && fc == 1 && bl == 1 && br == 1)
            return "5.0";
        else if (channels == 6 && fl == 1 && fr == 1 && fc == 1 && lfe == 1 && bl == 1 && br == 1)
            return "5.1";
        else if (channels == 7 && fl == 1 && fr == 1 && fc == 1 && lfe == 1 && bl == 1 && br == 1 && bc == 1)
            return "6.1";
        else if (channels == 8 && fl == 1 && fr == 1 && fc == 1 && lfe == 1 && bl == 1 && br == 1 && sl == 1 && sr == 1)
            return "7.1";
        // Atmos formats (with height channels)
        else if (channels == 8 && fl == 1 && fr == 1 && fc == 1 && lfe == 1 && bl == 1 && br == 1 && tfl == 1 && tfr == 1)
            return "5.1.2 (Atmos)";
        else if (channels == 10 && fl == 1 && fr == 1 && fc == 1 && lfe == 1 && bl == 1 && br == 1 && tfl == 1 && tfr == 1 && tbl == 1 && tbr == 1)
            return "5.1.4 (Atmos)";
        else if (channels == 10 && fl == 1 && fr == 1 && fc == 1 && lfe == 1 && sl == 1 && sr == 1 && bl == 1 && br == 1 && tfl == 1 && tfr == 1)
            return "7.1.2 (Atmos)";
        else if (channels == 12 && fl == 1 && fr == 1 && fc == 1 && lfe == 1 && sl == 1 && sr == 1 && bl == 1 && br == 1 &&
                 tfl == 1 && tfr == 1 && tbl == 1 && tbr == 1)
            return "7.1.4 (Atmos)";

        return "";  // Unknown/custom configuration
    }

    // Helper to get layout name from port type and channel count
    std::string getLayoutNameFromPortInfo(const clap_audio_port_info_t& info) {
        if (info.port_type != nullptr) {
            if (strcmp(info.port_type, CLAP_PORT_MONO) == 0)
                return "Mono";
            else if (strcmp(info.port_type, CLAP_PORT_STEREO) == 0)
                return "Stereo";
            else if (strcmp(info.port_type, CLAP_PORT_SURROUND) == 0) {
                // Will be refined with surround extension if available
                return "";
            }
        }

        // Fallback based on channel count
        switch (info.channel_count) {
            case 1: return "Mono";
            case 2: return "Stereo";
            default: return "";
        }
    }
}

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
        auto rawPlugin = plugin->clapPlugin();

        // Check for audio-ports-config extension
        auto* portsConfigExt = rawPlugin ?
            (const clap_plugin_audio_ports_config_t*)rawPlugin->get_extension(rawPlugin, CLAP_EXT_AUDIO_PORTS_CONFIG) : nullptr;

        // Check for surround extension
        auto* surroundExt = rawPlugin ?
            (const clap_plugin_surround_t*)rawPlugin->get_extension(rawPlugin, CLAP_EXT_SURROUND) : nullptr;
        auto* portsConfigInfoExt = rawPlugin ?
            (const clap_plugin_audio_ports_config_info_t*) rawPlugin->get_extension(rawPlugin, CLAP_EXT_AUDIO_PORTS_CONFIG_INFO) : nullptr;
        if (!portsConfigInfoExt && rawPlugin)
            portsConfigInfoExt = (const clap_plugin_audio_ports_config_info_t*) rawPlugin->get_extension(rawPlugin, CLAP_EXT_AUDIO_PORTS_CONFIG_INFO_COMPAT);

        if (plugin && plugin->canUseAudioPorts()) {
            std::vector<clap_audio_port_info_t> inputPortInfos;
            std::vector<clap_audio_port_info_t> outputPortInfos;

            for (bool isInput : {true, false}) {
                auto& portInfos = isInput ? inputPortInfos : outputPortInfos;
                for (uint32_t i = 0, n = plugin->audioPortsCount(isInput); i < n; i++) {
                    clap_audio_port_info_t info{};
                    if (!plugin->audioPortsGet(i, isInput, &info))
                        continue;
                    portInfos.emplace_back(info);
                }
            }

            // Store port infos for later use (in-place processing detection)
            owner->inputPortInfos = inputPortInfos;
            owner->outputPortInfos = outputPortInfos;

            // Helper to build bus definitions with surround support
            auto buildBusDefs = [&](const std::vector<clap_audio_port_info_t>& portInfos,
                                    std::vector<AudioBusDefinition>& busDefs,
                                    uint32_t& busCount,
                                    bool isInput) {
                for (size_t i = 0; i < portInfos.size(); i++) {
                    const auto& info = portInfos[i];
                    std::string layoutName = getLayoutNameFromPortInfo(info);

                    // Try to get more precise layout from surround extension
                    if (surroundExt && info.port_type && strcmp(info.port_type, CLAP_PORT_SURROUND) == 0) {
                        std::vector<uint8_t> channelMap(info.channel_count);
                        uint32_t retrieved = surroundExt->get_channel_map(rawPlugin, isInput, static_cast<uint32_t>(i),
                                                                          channelMap.data(), info.channel_count);
                        if (retrieved == info.channel_count) {
                            std::string surroundLayoutName = getLayoutNameFromChannelMap(channelMap);
                            if (!surroundLayoutName.empty())
                                layoutName = surroundLayoutName;
                        }
                    }

                    std::vector<AudioChannelLayout> layouts{};
                    AudioChannelLayout layout{layoutName, info.channel_count};
                    layouts.emplace_back(layout);

                    AudioBusDefinition def{info.name,
                                          info.flags & CLAP_AUDIO_PORT_IS_MAIN ? AudioBusRole::Main : AudioBusRole::Aux,
                                          layouts};
                    busCount++;
                    busDefs.emplace_back(def);
                }
            };

            buildBusDefs(inputPortInfos, input_bus_defs, ret.numAudioIn, true);
            buildBusDefs(outputPortInfos, output_bus_defs, ret.numAudioOut, false);
        } else if (portsConfigExt && portsConfigInfoExt) {
            auto selectConfigId = [&]() -> std::optional<clap_id> {
                if (!rawPlugin)
                    return std::nullopt;
                const auto current = portsConfigInfoExt->current_config(rawPlugin);
                if (current != CLAP_INVALID_ID)
                    return current;
                uint32_t configCount = portsConfigExt->count(rawPlugin);
                for (uint32_t i = 0; i < configCount; ++i) {
                    clap_audio_ports_config_t cfg{};
                    if (portsConfigExt->get(rawPlugin, i, &cfg))
                        return cfg.id;
                }
                return std::nullopt;
            };

            auto configId = selectConfigId();
            if (configId.has_value()) {
                clap_audio_ports_config_t cfg{};
                uint32_t configCount = portsConfigExt->count(rawPlugin);
                for (uint32_t i = 0; i < configCount; ++i) {
                    clap_audio_ports_config_t tmp{};
                    if (portsConfigExt->get(rawPlugin, i, &tmp) && tmp.id == configId.value()) {
                        cfg = tmp;
                        break;
                    }
                }

                auto buildFromConfig = [&](uint32_t portCount,
                                           bool isInput,
                                           std::vector<AudioBusDefinition>& busDefs,
                                           uint32_t& busCounter) {
                    for (uint32_t port = 0; port < portCount; ++port) {
                        clap_audio_port_info_t info{};
                        if (!portsConfigInfoExt->get(rawPlugin, configId.value(), port, isInput, &info))
                            continue;

                        std::string layoutName = getLayoutNameFromPortInfo(info);
                        if (surroundExt && info.port_type && strcmp(info.port_type, CLAP_PORT_SURROUND) == 0) {
                            std::vector<uint8_t> channelMap(info.channel_count);
                            uint32_t retrieved = surroundExt->get_channel_map(rawPlugin, isInput, port,
                                                                              channelMap.data(), info.channel_count);
                            if (retrieved == info.channel_count) {
                                std::string surroundLayoutName = getLayoutNameFromChannelMap(channelMap);
                                if (!surroundLayoutName.empty())
                                    layoutName = surroundLayoutName;
                            }
                        }

                        std::vector<AudioChannelLayout> layouts{};
                        layouts.emplace_back(AudioChannelLayout{layoutName, info.channel_count});
                        AudioBusDefinition def{info.name,
                                               info.flags & CLAP_AUDIO_PORT_IS_MAIN ? AudioBusRole::Main : AudioBusRole::Aux,
                                               layouts};
                        busDefs.emplace_back(def);
                        ++busCounter;
                    }
                };

                buildFromConfig(cfg.input_port_count, true, input_bus_defs, ret.numAudioIn);
                buildFromConfig(cfg.output_port_count, false, output_bus_defs, ret.numAudioOut);

                if (input_bus_defs.empty() && cfg.has_main_input) {
                    std::string name = cfg.name;
                    if (!name.empty())
                        name += " Main Input";
                    std::vector<AudioChannelLayout> layouts{AudioChannelLayout{"", cfg.main_input_channel_count}};
                    input_bus_defs.emplace_back(AudioBusDefinition{name.empty() ? "Main Input" : name, AudioBusRole::Main, layouts});
                    ret.numAudioIn++;
                }
                if (output_bus_defs.empty() && cfg.has_main_output) {
                    std::string name = cfg.name;
                    if (!name.empty())
                        name += " Main Output";
                    std::vector<AudioChannelLayout> layouts{AudioChannelLayout{"", cfg.main_output_channel_count}};
                    output_bus_defs.emplace_back(AudioBusDefinition{name.empty() ? "Main Output" : name, AudioBusRole::Main, layouts});
                    ret.numAudioOut++;
                }
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
        auto plugin = owner->plugin.get();
        auto rawPlugin = plugin->clapPlugin();

        // Check for audio-ports-config extension
        auto* portsConfigExt = rawPlugin ?
            (const clap_plugin_audio_ports_config_t*)rawPlugin->get_extension(rawPlugin, CLAP_EXT_AUDIO_PORTS_CONFIG) : nullptr;

        bool refreshedBuses = false;

        // If audio-ports-config extension is available, try to select a matching configuration
        if (portsConfigExt) {
            std::optional<clap_id> selectedConfigId;

            if (configuration.mainInputChannels.has_value() || configuration.mainOutputChannels.has_value()) {
                uint32_t configCount = portsConfigExt->count(rawPlugin);
                for (uint32_t i = 0; i < configCount; i++) {
                    clap_audio_ports_config_t config{};
                    if (!portsConfigExt->get(rawPlugin, i, &config))
                        continue;

                    bool inputMatches = !config.has_main_input ||
                        (!configuration.mainInputChannels.has_value() ||
                         configuration.mainInputChannels.value() == config.main_input_channel_count);
                    bool outputMatches = !config.has_main_output ||
                        (!configuration.mainOutputChannels.has_value() ||
                         configuration.mainOutputChannels.value() == config.main_output_channel_count);

                    if (inputMatches && outputMatches) {
                        selectedConfigId = config.id;
                        break;
                    }
                }
            }

            if (selectedConfigId.has_value()) {
                if (portsConfigExt->select(rawPlugin, selectedConfigId.value())) {
                    inspectBuses();
                    refreshedBuses = true;
                }
            }
        }

        if (!refreshedBuses)
            inspectBuses();

        // Rebuild AudioBusConfiguration objects from the latest bus definitions
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
