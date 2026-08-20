#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace uapmd_mir {

using RhythmAnalysisLogger = std::function<void(std::string_view)>;

std::vector<std::tuple<double, uint8_t, uint8_t>> detectRhythmMap(
    std::span<const float> samples, int sampleRate,
    const std::vector<std::pair<double, double>>& tempoPoints, double fallbackBpm,
    const RhythmAnalysisLogger& logger = {});

} // namespace uapmd_mir
