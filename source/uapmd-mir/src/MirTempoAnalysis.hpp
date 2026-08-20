#pragma once

#include <span>
#include <cstdint>
#include <functional>
#include <string_view>
#include <utility>
#include <vector>

namespace uapmd_mir {

using AnalysisLogger = std::function<void(std::string_view)>;

// Times are seconds from the start of the analyzed audio. BPM values are
// selected directly from libsonare's local analysis results.
std::vector<std::pair<double, double>> detectTempoMap(
    std::span<const float> samples, int sampleRate, double fallbackBpm,
    const AnalysisLogger& logger = {});

uint64_t tempoMapTimeToTicks(
    std::span<const std::pair<double, double>> tempoPoints,
    double seconds, uint32_t tickResolution, double fallbackBpm);

} // namespace uapmd_mir
