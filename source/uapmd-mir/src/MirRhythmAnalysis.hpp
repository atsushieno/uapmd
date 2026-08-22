#pragma once

#include <cstdint>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

#include "MirTempoMap.hpp"

namespace uapmd_mir::sonare {

// libsonare-backed meter map. Beats are tracked per tempo-map segment; the
// segmentation itself lives in MirMeterMap.
std::vector<std::tuple<double, uint8_t, uint8_t>> detectRhythmMap(
    std::span<const float> samples, int sampleRate,
    const std::vector<std::pair<double, double>>& tempoPoints, double fallbackBpm,
    const AnalysisLogger& logger = {});

} // namespace uapmd_mir::sonare
