#include "PluginFormatLV2.hpp"
#include <optional>
#include <algorithm>
#include <vector>
#include <map>

namespace {
    // Helper to get channel layout name based on port designations
    std::string getLayoutName(const std::vector<const LilvNode*>& designations, remidy_lv2::LV2ImplPluginContext& implContext) {
        auto& statics = implContext.statics;

        if (designations.empty())
            return "";

        // Count channel types
        int left = 0, right = 0, center = 0, rearLeft = 0, rearRight = 0, rearCenter = 0, lfe = 0;
        int centerLeft = 0, centerRight = 0;

        for (auto node : designations) {
            if (lilv_node_equals(node, statics->pg_left_uri_node)) left++;
            else if (lilv_node_equals(node, statics->pg_right_uri_node)) right++;
            else if (lilv_node_equals(node, statics->pg_center_uri_node)) center++;
            else if (lilv_node_equals(node, statics->pg_rear_left_uri_node)) rearLeft++;
            else if (lilv_node_equals(node, statics->pg_rear_right_uri_node)) rearRight++;
            else if (lilv_node_equals(node, statics->pg_rear_center_uri_node)) rearCenter++;
            else if (lilv_node_equals(node, statics->pg_lfe_uri_node)) lfe++;
            else if (lilv_node_equals(node, statics->pg_center_left_uri_node)) centerLeft++;
            else if (lilv_node_equals(node, statics->pg_center_right_uri_node)) centerRight++;
        }

        int totalChannels = static_cast<int>(designations.size());

        // Identify common configurations
        if (totalChannels == 1) {
            return "Mono";
        } else if (totalChannels == 2 && left == 1 && right == 1) {
            return "Stereo";
        } else if (totalChannels == 3 && left == 1 && right == 1 && center == 1) {
            return "3.0 (L R C)";
        } else if (totalChannels == 4 && left == 1 && right == 1 && rearLeft == 1 && rearRight == 1) {
            return "Quadraphonic";
        } else if (totalChannels == 5 && left == 1 && right == 1 && center == 1 && rearLeft == 1 && rearRight == 1) {
            return "5.0";
        } else if (totalChannels == 6 && left == 1 && right == 1 && center == 1 && lfe == 1 && rearLeft == 1 && rearRight == 1) {
            return "5.1";
        } else if (totalChannels == 7 && center == 1 && lfe == 1 && rearCenter == 1) {
            return "6.1";
        } else if (totalChannels == 8 && left == 1 && right == 1 && center == 1 && lfe == 1 &&
                   rearLeft == 1 && rearRight == 1 && (centerLeft == 1 || rearLeft == 2) && (centerRight == 1 || rearRight == 2)) {
            return "7.1";
        }

        return "";  // Unknown/custom configuration
    }
}

void remidy::PluginInstanceLV2::AudioBuses::inspectBuses() {
    auto plugin = owner->plugin;
    auto& implContext = owner->implContext;

    BusSearchResult ret{};

    input_bus_defs.clear();
    output_bus_defs.clear();
    audio_in_buses.clear();
    audio_out_buses.clear();

    // Map to track port groups and their channel designations
    std::map<std::string, std::vector<const LilvNode*>> inputGroupDesignations;
    std::map<std::string, std::vector<const LilvNode*>> outputGroupDesignations;
    std::map<std::string, bool> groupIsSideChain;

    // First pass: collect all ports and their designations per group
    for (uint32_t p = 0, n = lilv_plugin_get_num_ports(plugin); p < n; p++) {
        auto port = lilv_plugin_get_port_by_index(plugin, p);
        if (implContext.IS_AUDIO_PORT(plugin, port)) {
            bool isInput = implContext.IS_INPUT_PORT(plugin, port);
            auto groupNode = lilv_port_get(plugin, port, implContext.statics->port_group_uri_node);
            std::string group = groupNode ? lilv_node_as_string(groupNode) : "";

            auto scNode = lilv_port_get(plugin, port, implContext.statics->is_side_chain_uri_node);
            bool isSideChain = scNode != nullptr && lilv_node_as_bool(scNode);
            groupIsSideChain[group] = isSideChain;

            // Get port designation
            auto designationNode = lilv_port_get(plugin, port, implContext.statics->designation_uri_node);

            auto& groupDes = isInput ? inputGroupDesignations[group] : outputGroupDesignations[group];
            groupDes.push_back(designationNode);
        }
        if (implContext.IS_ATOM_IN(plugin, port))
            ret.numEventIn++;
        if (implContext.IS_ATOM_OUT(plugin, port))
            ret.numEventOut++;
    }

    // Second pass: create bus definitions with proper channel layouts
    auto createBuses = [&](std::map<std::string, std::vector<const LilvNode*>>& groupDes,
                           std::vector<AudioBusDefinition>& busDefs,
                           std::vector<AudioBusConfiguration*>& buses,
                           uint32_t& busCount) {
        for (auto& [groupName, designations] : groupDes) {
            uint32_t channels = static_cast<uint32_t>(designations.size());
            std::string layoutName = getLayoutName(designations, implContext);

            bool isSideChain = groupIsSideChain[groupName];
            auto def = AudioBusDefinition(groupName, isSideChain ? AudioBusRole::Aux : AudioBusRole::Main);
            busDefs.emplace_back(def);

            auto bus = new AudioBusConfiguration(def);

            // Set appropriate channel layout based on detected configuration
            if (!layoutName.empty()) {
                bus->channelLayout(AudioChannelLayout{layoutName, channels});
            } else {
                // Fallback for custom configurations
                switch (channels) {
                    case 1:
                        bus->channelLayout(AudioChannelLayout::mono());
                        break;
                    case 2:
                        bus->channelLayout(AudioChannelLayout::stereo());
                        break;
                    default:
                        bus->channelLayout(AudioChannelLayout{"", channels});
                        break;
                }
            }

            buses.emplace_back(bus);
            busCount++;
        }
    };

    createBuses(inputGroupDesignations, input_bus_defs, audio_in_buses, ret.numAudioIn);
    createBuses(outputGroupDesignations, output_bus_defs, audio_out_buses, ret.numAudioOut);

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
