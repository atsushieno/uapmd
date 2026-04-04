#include "uapmd-data/uapmd-data.hpp"
#include "UapmdAudioPluginFullDAGraphData.hpp"

#include <optional>

namespace uapmd {

namespace {

std::optional<AudioPluginGraphEndpoint> toRuntimeEndpoint(
    const UapmdProjectPluginGraphEndpointData& endpoint,
    const std::vector<int32_t>& orderedInstanceIds
) {
    AudioPluginGraphEndpoint result;
    result.type = endpoint.type;
    result.bus_index = endpoint.bus_index;
    if (endpoint.type == AudioPluginGraphEndpointType::Plugin) {
        if (endpoint.plugin_index < 0 || endpoint.plugin_index >= static_cast<int32_t>(orderedInstanceIds.size()))
            return std::nullopt;
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

    fullGraph->clearConnections();
    for (const auto& serialized : dagData->connections()) {
        auto source = toRuntimeEndpoint(serialized.source, orderedInstanceIds);
        auto target = toRuntimeEndpoint(serialized.target, orderedInstanceIds);
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
