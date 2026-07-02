#pragma once

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace uapmd::gui {

struct LaneAssignmentInput {
    int32_t id;
    int32_t start;
    int32_t end;
};

// Greedy lane assignment: sort clips by start, then assign each to the first lane whose last
// clip ends at or before this clip's start. This distributes overlapping clips into separate
// vertical lanes within the same track section. Unit-agnostic — `start`/`end` may be seconds-
// frames or beat-ticks, as long as both are in the same unit.
inline std::unordered_map<int32_t, int> assignLanes(
    const std::vector<LaneAssignmentInput>& clips,
    int& outLaneCount
) {
    std::unordered_map<int32_t, int> laneByClipId;
    std::vector<int32_t> laneEndValues; // last-used end value per lane

    std::vector<const LaneAssignmentInput*> sorted;
    sorted.reserve(clips.size());
    for (const auto& c : clips)
        sorted.push_back(&c);
    std::sort(sorted.begin(), sorted.end(),
        [](const LaneAssignmentInput* a, const LaneAssignmentInput* b) { return a->start < b->start; });

    for (const auto* c : sorted) {
        int lane = -1;
        for (int l = 0; l < static_cast<int>(laneEndValues.size()); ++l) {
            if (laneEndValues[l] <= c->start) {
                lane = l;
                laneEndValues[l] = c->end;
                break;
            }
        }
        if (lane < 0) {
            lane = static_cast<int>(laneEndValues.size());
            laneEndValues.push_back(c->end);
        }
        laneByClipId[c->id] = lane;
    }

    outLaneCount = std::max(1, static_cast<int>(laneEndValues.size()));
    return laneByClipId;
}

} // namespace uapmd::gui
