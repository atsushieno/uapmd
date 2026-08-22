#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "MirTempoMap.hpp"

namespace uapmd_mir::librosa_cpp {

// librosa.cpp-backed analysis. The window scheduling, run grouping, transition
// refinement and meter segmentation are shared with the libsonare backend (see
// MirTempoMap / MirMeterMap); only the primitives below are librosa's.

std::vector<std::pair<double, double>> detectTempoMap(
    std::span<const float> samples, int sampleRate, double fallbackBpm,
    const AnalysisLogger& logger = {});

std::vector<std::tuple<double, uint8_t, uint8_t>> detectRhythmMap(
    std::span<const float> samples, int sampleRate,
    const std::vector<std::pair<double, double>>& tempoPoints, double fallbackBpm,
    const AnalysisLogger& logger = {});

std::vector<std::pair<double, std::string>> detectChords(
    std::span<const float> samples, int sampleRate, const AnalysisLogger& logger = {});

} // namespace uapmd_mir::librosa_cpp
