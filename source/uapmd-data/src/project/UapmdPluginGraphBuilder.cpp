#include "uapmd-data/uapmd-data.hpp"

namespace uapmd {

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
        graph.appendBuiltInNodeSimple(node);
    }
    return true;
}

} // namespace uapmd
