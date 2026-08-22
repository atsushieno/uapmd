#pragma once

#include <span>
#include <utility>
#include <vector>

#include "MirTempoMap.hpp"

namespace uapmd_mir::sonare {

// libsonare-backed tempo map. The window scheduling, run grouping and
// transition refinement live in MirTempoMap; this only supplies libsonare's
// per-window BPM estimate and onset envelope.
std::vector<std::pair<double, double>> detectTempoMap(
    std::span<const float> samples, int sampleRate, double fallbackBpm,
    const AnalysisLogger& logger = {});

} // namespace uapmd_mir::sonare
