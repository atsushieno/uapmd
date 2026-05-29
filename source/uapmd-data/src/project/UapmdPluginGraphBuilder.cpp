#include "uapmd-data/uapmd-data.hpp"

#include <string_view>

namespace uapmd {

namespace {

constexpr std::string_view kImplicitTrackGainNodeId = "builtin:track_gain";

}

bool UapmdPluginGraphBuilder::build(
    UapmdProjectPluginGraphData* data,
    AudioPluginGraph& graph,
    const std::vector<int32_t>& orderedInstanceIds
) {
    (void) orderedInstanceIds;
    if (!data)
        return false;

    for (const auto& node : data->genericNodes()) {
        if (node.plugin)
            continue;
        if (node.node_id == kImplicitTrackGainNodeId)
            continue;
        graph.appendBuiltInNodeSimple(node);
    }
    return true;
}

} // namespace uapmd
