#include "MirTempoAnalysis.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <optional>

#include <sonare/sonare_c.h>

namespace uapmd_mir::sonare {
namespace {

constexpr int kOnsetHopLength = 512;

} // namespace

std::vector<std::pair<double, double>> detectTempoMap(
        std::span<const float> samples, int sampleRate, double fallbackBpm,
        const AnalysisLogger& logger) {
    if (samples.empty() || sampleRate <= 0)
        return {{0.0, fallbackBpm}};

    const auto duration = static_cast<double>(samples.size()) / sampleRate;

    float* onsetData = nullptr;
    size_t onsetLength = 0;
    if (logger)
        logger("uapmd calling libsonare onset envelope");
    const auto onsetError = sonare_onset_strength(
        samples.data(), samples.size(), sampleRate, 2048, kOnsetHopLength, 128,
        &onsetData, &onsetLength);
    OnsetEnvelope onset{};
    if (onsetError == SONARE_OK) {
        onset.strengths = {onsetData, onsetLength};
        onset.framesPerSecond = static_cast<double>(sampleRate) / kOnsetHopLength;
    }

    const TempoWindowEstimator estimateWindow =
        [&](double start, double end) -> std::optional<TempoWindowEstimate> {
        const auto firstSample = static_cast<size_t>(std::llround(start * sampleRate));
        const auto lastSample = std::min(samples.size(), static_cast<size_t>(
            std::llround(end * sampleRate)));
        if (lastSample <= firstSample)
            return std::nullopt;

        if (logger)
            logger(std::format(
                "uapmd calling libsonare BPM window start={:.6f}s end={:.6f}s",
                start, end));
        SonareBpmAnalysisResult analysis{};
        const auto error = sonare_analyze_bpm(
            samples.data() + firstSample, lastSample - firstSample, sampleRate,
            40.0f, 240.0f, static_cast<float>(fallbackBpm), 4096, 512, 10, &analysis);
        if (error != SONARE_OK) {
            sonare_free_bpm_analysis_result(&analysis);
            return std::nullopt;
        }

        TempoWindowEstimate estimate{analysis.bpm, analysis.confidence, {}};
        estimate.candidates.reserve(analysis.candidate_count);
        for (size_t index = 0; index < analysis.candidate_count; ++index)
            estimate.candidates.emplace_back(analysis.candidates[index].bpm,
                                             analysis.candidates[index].confidence);
        if (logger) {
            logger(std::format(
                "libsonare raw BPM window start={:.6f}s end={:.6f}s bpm={:.6f} confidence={:.6f}",
                start, end, analysis.bpm, analysis.confidence));
            for (size_t index = 0; index < analysis.candidate_count; ++index)
                logger(std::format(
                    "libsonare raw BPM candidate window_start={:.6f}s index={} bpm={:.6f} confidence={:.6f}",
                    start, index, analysis.candidates[index].bpm,
                    analysis.candidates[index].confidence));
        }
        sonare_free_bpm_analysis_result(&analysis);
        return estimate;
    };

    auto result = buildTempoMap(duration, estimateWindow, onset, fallbackBpm, logger);
    sonare_free_floats(onsetData);
    return result;
}

} // namespace uapmd_mir::sonare
