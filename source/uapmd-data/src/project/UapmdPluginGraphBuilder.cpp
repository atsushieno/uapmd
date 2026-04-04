#include "uapmd-data/uapmd-data.hpp"

namespace uapmd {

bool UapmdPluginGraphBuilder::build(
    UapmdProjectPluginGraphData* data,
    AudioPluginGraph& graph,
    const std::vector<int32_t>& orderedInstanceIds
) {
    (void) graph;
    (void) orderedInstanceIds;
    return data != nullptr;
}

} // namespace uapmd
