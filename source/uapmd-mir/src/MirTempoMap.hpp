#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace uapmd_mir {

using AnalysisLogger = std::function<void(std::string_view)>;

// Analysis window geometry. Backends need these to decide whether a source is
// long enough to analyze at all, so they are part of the interface.
inline constexpr double kTempoWindowSeconds = 12.0;
inline constexpr double kTempoWindowHopSeconds = 3.0;
inline constexpr double kMinimumTempoWindowSeconds = 8.0;

// What a backend reports for one analysis window. `candidates` is ordered best
// first and holds (BPM, score) pairs; scores only have to be comparable within
// one window, because every rule that reads them compares against the window's
// own best score.
struct TempoWindowEstimate {
    double bpm{};
    double confidence{};
    std::vector<std::pair<double, double>> candidates;
};

// Estimates the tempo over [startSeconds, endSeconds). Returning nullopt drops
// the window instead of failing the analysis.
using TempoWindowEstimator =
    std::function<std::optional<TempoWindowEstimate>(double startSeconds, double endSeconds)>;

// Onset strength envelope over the whole source, with its frame rate. Used to
// place tempo transitions more precisely than the window grid allows.
struct OnsetEnvelope {
    std::span<const float> strengths;
    double framesPerSecond{0.0};
};

// Times are seconds from the start of the analyzed audio. BPM values are
// selected directly from the backend's local analysis results.
std::vector<std::pair<double, double>> buildTempoMap(
    double durationSeconds, const TempoWindowEstimator& estimateWindow,
    const OnsetEnvelope& onset, double fallbackBpm, const AnalysisLogger& logger = {});

uint64_t tempoMapTimeToTicks(
    std::span<const std::pair<double, double>> tempoPoints,
    double seconds, uint32_t tickResolution, double fallbackBpm);

} // namespace uapmd_mir
