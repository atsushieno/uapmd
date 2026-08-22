#pragma once

#include <cstdint>
#include <span>
#include <tuple>
#include <vector>

#include "MirTempoMap.hpp"

namespace uapmd_mir {

// Per-beat accent strength sampled from an onset envelope: the maximum over
// +/-2 envelope frames around each beat, min-max normalized across the track.
std::vector<float> beatStrengths(
    std::span<const float> beatSeconds, const OnsetEnvelope& onset);

// Segments the beat sequence into time-signature regions. Beat times are
// seconds from the start of the analyzed audio; the returned times are the
// first beat of each region, which is thereby asserted to be its downbeat.
std::vector<std::tuple<double, uint8_t, uint8_t>> estimateMeterMap(
    std::span<const float> beats, std::span<const float> strengths,
    const AnalysisLogger& logger = {});

} // namespace uapmd_mir
