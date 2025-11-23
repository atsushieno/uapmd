#include "PluginFormatCLAP.hpp"
#include <clap/ext/audio-ports.h>
#include <clap/ext/audio-ports-config.h>
#include <clap/ext/surround.h>
#include <clap/ext/note-ports.h>
#include <optional>
#include <map>

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

namespace {
    // Helper to store config ID with layout name (format: "name|configId")
    std::string makeLayoutNameWithConfigId(const std::string& name, clap_id configId) {
        return name + "|" + std::to_string(configId);
    }

    // Helper to extract config ID from layout name
    std::optional<clap_id> extractConfigIdFromLayoutName(const std::string& layoutName) {
        auto pos = layoutName.rfind('|');
        if (pos == std::string::npos)
            return std::nullopt;

        try {
            return std::stoul(layoutName.substr(pos + 1));
        } catch (...) {
            return std::nullopt;
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

        if (plugin && plugin->canUseAudioPorts()) {
            // If audio-ports-config extension is available, enumerate all configurations
            if (portsConfigExt) {
                uint32_t configCount = portsConfigExt->count(rawPlugin);

                // Build bus definitions from all available configurations
                std::map<std::string, std::vector<AudioChannelLayout>> mainInputLayouts;
                std::map<std::string, std::vector<AudioChannelLayout>> mainOutputLayouts;

                for (uint32_t i = 0; i < configCount; i++) {
                    clap_audio_ports_config_t config;
                    if (!portsConfigExt->get(rawPlugin, i, &config))
                        continue;

                    // Create layout for this configuration
                    if (config.has_main_input) {
                        std::string layoutName = config.main_input_port_type ?
                            std::string(config.main_input_port_type) : "";
                        if (layoutName.empty())
                            layoutName = config.main_input_channel_count == 1 ? "Mono" :
                                        config.main_input_channel_count == 2 ? "Stereo" : "";

                        std::string fullName = config.name;
                        if (!layoutName.empty())
                            fullName += " (" + layoutName + ")";

                        mainInputLayouts["Main Input"].emplace_back(
                            AudioChannelLayout{makeLayoutNameWithConfigId(fullName, config.id),
                                             config.main_input_channel_count});
                    }

                    if (config.has_main_output) {
                        std::string layoutName = config.main_output_port_type ?
                            std::string(config.main_output_port_type) : "";
                        if (layoutName.empty())
                            layoutName = config.main_output_channel_count == 1 ? "Mono" :
                                        config.main_output_channel_count == 2 ? "Stereo" : "";

                        std::string fullName = config.name;
                        if (!layoutName.empty())
                            fullName += " (" + layoutName + ")";

                        mainOutputLayouts["Main Output"].emplace_back(
                            AudioChannelLayout{makeLayoutNameWithConfigId(fullName, config.id),
                                             config.main_output_channel_count});
                    }
                }

                // Create bus definitions with all available layouts
                if (!mainInputLayouts.empty()) {
                    for (auto& [busName, layouts] : mainInputLayouts) {
                        AudioBusDefinition def{busName, AudioBusRole::Main, layouts};
                        input_bus_defs.emplace_back(def);
                        ret.numAudioIn++;
                    }
                }

                if (!mainOutputLayouts.empty()) {
                    for (auto& [busName, layouts] : mainOutputLayouts) {
                        AudioBusDefinition def{busName, AudioBusRole::Main, layouts};
                        output_bus_defs.emplace_back(def);
                        ret.numAudioOut++;
                    }
                }
            } else {
                // No audio-ports-config extension - use direct audio-ports enumeration
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

        // If audio-ports-config extension is available, try to select a matching configuration
        if (portsConfigExt) {
            // Look at the current bus configurations to find if a specific config was selected
            std::optional<clap_id> selectedConfigId;

            // Check main input bus for config ID
            if (!input_bus_defs.empty()) {
                auto& mainInputDef = input_bus_defs[0];  // Assuming first is main
                const auto& layouts = mainInputDef.supportedChannelLayouts();
                if (!layouts.empty()) {
                    // Check if any layout has a config ID embedded
                    for (auto& layout : layouts) {
                        auto configId = extractConfigIdFromLayoutName(const_cast<AudioChannelLayout&>(layout).name());
                        if (configId.has_value()) {
                            // User might have selected this layout - check if it matches requested config
                            selectedConfigId = configId;
                            break;
                        }
                    }
                }
            }

            // If no config ID found yet, check main output bus
            if (!selectedConfigId.has_value() && !output_bus_defs.empty()) {
                auto& mainOutputDef = output_bus_defs[0];
                const auto& layouts = mainOutputDef.supportedChannelLayouts();
                if (!layouts.empty()) {
                    for (auto& layout : layouts) {
                        auto configId = extractConfigIdFromLayoutName(const_cast<AudioChannelLayout&>(layout).name());
                        if (configId.has_value()) {
                            selectedConfigId = configId;
                            break;
                        }
                    }
                }
            }

            // Try to match requested configuration to a config preset
            if (!selectedConfigId.has_value() && configuration.mainInputChannels.has_value() && configuration.mainOutputChannels.has_value()) {
                uint32_t configCount = portsConfigExt->count(rawPlugin);
                for (uint32_t i = 0; i < configCount; i++) {
                    clap_audio_ports_config_t config;
                    if (!portsConfigExt->get(rawPlugin, i, &config))
                        continue;

                    // Match input and output channel counts
                    bool inputMatches = !config.has_main_input ||
                                       (configuration.mainInputChannels.value() == config.main_input_channel_count);
                    bool outputMatches = !config.has_main_output ||
                                        (configuration.mainOutputChannels.value() == config.main_output_channel_count);

                    if (inputMatches && outputMatches) {
                        selectedConfigId = config.id;
                        break;
                    }
                }
            }

            // Apply the selected configuration
            if (selectedConfigId.has_value()) {
                if (portsConfigExt->select(rawPlugin, selectedConfigId.value())) {
                    // Configuration applied successfully - re-inspect buses to get actual port layout
                    inspectBuses();
                    return;
                }
            }
        }

        // Fallback: standard bus configuration without audio-ports-config extension
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
