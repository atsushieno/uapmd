#include "uapmd-data/uapmd-data.hpp"
#include "UapmdAudioPluginFullDAGraphData.hpp"

#include <optional>
#include <string_view>

namespace uapmd {

namespace {

constexpr std::string_view kGraphInputNodeId = "graph:input";
constexpr std::string_view kGraphOutputNodeId = "graph:output";
constexpr std::string_view kImplicitTrackGainNodeId = "builtin:track_gain";

std::optional<AudioPluginGraphEndpoint> toRuntimeEndpoint(
    const UapmdProjectPluginGraphEndpointData& endpoint,
    AudioPluginGraph& graph,
    const std::vector<int32_t>& orderedInstanceIds
) {
    AudioPluginGraphEndpoint result;
    result.type = endpoint.type;
    result.node_id = endpoint.node_id;
    result.bus_index = endpoint.bus_index;

    if (!endpoint.node_id.empty()) {
        if (endpoint.node_id == kGraphInputNodeId) {
            result.type = AudioPluginGraphEndpointType::GraphInput;
            return result;
        }
        if (endpoint.node_id == kGraphOutputNodeId) {
            result.type = AudioPluginGraphEndpointType::GraphOutput;
            return result;
        }
        if (auto* node = dynamic_cast<AudioPluginNode*>(graph.getNode(endpoint.node_id))) {
            result.type = AudioPluginGraphEndpointType::Plugin;
            result.node_id = endpoint.node_id;
            result.instance_id = node->instanceId();
            return result;
        }
        result.type = AudioPluginGraphEndpointType::Plugin;
        return result;
    }

    if (endpoint.type == AudioPluginGraphEndpointType::Plugin) {
        if (endpoint.plugin_index < 0 || endpoint.plugin_index >= static_cast<int32_t>(orderedInstanceIds.size()))
            return std::nullopt;
        result.node_id = "plugin:" + std::to_string(orderedInstanceIds[static_cast<size_t>(endpoint.plugin_index)]);
        result.instance_id = orderedInstanceIds[static_cast<size_t>(endpoint.plugin_index)];
    }
    return result;
}

} // namespace

bool UapmdAudioPluginFullDAGraphBuilder::build(
    UapmdProjectPluginGraphData* data,
    AudioPluginGraph& graph,
    const std::vector<int32_t>& orderedInstanceIds
) {
    auto* dagData = dynamic_cast<UapmdAudioPluginFullDAGraphData*>(data);
    if (!dagData)
        return false;

    auto* fullGraph = dynamic_cast<AudioPluginFullDAGraph*>(&graph);
    if (!fullGraph)
        return false;

    for (const auto& node : dagData->genericNodes()) {
        if (node.plugin)
            continue;
        if (node.node_id == kImplicitTrackGainNodeId)
            continue;
        fullGraph->appendBuiltInNodeSimple(node);
    }

    fullGraph->clearConnections();
    for (const auto& serialized : dagData->connections()) {
        auto source = toRuntimeEndpoint(serialized.source, graph, orderedInstanceIds);
        auto target = toRuntimeEndpoint(serialized.target, graph, orderedInstanceIds);
        if (!source || !target)
            continue;
        fullGraph->connect(AudioPluginGraphConnection{
            .id = serialized.id,
            .bus_type = serialized.bus_type,
            .source = *source,
            .target = *target,
        });
    }
    return true;
}

} // namespace uapmd
