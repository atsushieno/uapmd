#include "MirRhythmAnalysis.hpp"
#include "MirMeterMap.hpp"

#include <algorithm>
#include <cmath>
#include <format>

#include <sonare/sonare_c.h>

namespace uapmd_mir::sonare {
namespace {

constexpr int kOnsetHopLength = 512;

std::vector<float> detectTempoAwareBeats(
        std::span<const float> samples, int sampleRate,
        const std::vector<std::pair<double, double>>& tempoPoints,
        const AnalysisLogger& logger) {
    const auto duration = static_cast<double>(samples.size()) / sampleRate;
    std::vector<float> result;
    for (size_t segment = 0; segment < tempoPoints.size(); ++segment) {
        const auto start = std::max(0.0, tempoPoints[segment].first);
        const auto end = segment + 1 < tempoPoints.size()
            ? std::min(duration, tempoPoints[segment + 1].first) : duration;
        if (end <= start)
            continue;
        const auto firstSample = static_cast<size_t>(std::llround(start * sampleRate));
        const auto lastSample = std::min(samples.size(), static_cast<size_t>(
            std::llround(end * sampleRate)));
        float* localData = nullptr;
        size_t localCount = 0;
        if (logger)
            logger(std::format(
                "uapmd calling libsonare beats section_start={:.6f}s section_end={:.6f}s",
                start, end));
        if (sonare_detect_beats(samples.data() + firstSample, lastSample - firstSample,
                                sampleRate, &localData, &localCount) == SONARE_OK) {
            if (logger)
                logger(std::format(
                    "libsonare raw beats section_start={:.6f}s section_end={:.6f}s count={}",
                    start, end, localCount));
            for (size_t index = 0; index < localCount; ++index) {
                const auto time = static_cast<float>(start + localData[index]);
                if (logger)
                    logger(std::format(
                        "libsonare raw beat section_start={:.6f}s index={} local={:.6f}s absolute={:.6f}s",
                        start, index, localData[index], time));
                if ((result.empty() || time > result.back() + 0.05f) && time < end)
                    result.push_back(time);
            }
        }
        sonare_free_floats(localData);
    }
    return result;
}

} // namespace

std::vector<std::tuple<double, uint8_t, uint8_t>> detectRhythmMap(
        std::span<const float> samples, int sampleRate,
        const std::vector<std::pair<double, double>>& tempoPoints, double,
        const AnalysisLogger& logger) {
    const auto beats = detectTempoAwareBeats(samples, sampleRate, tempoPoints, logger);
    float* onsetData = nullptr;
    size_t onsetCount = 0;
    if (logger)
        logger("uapmd calling libsonare onset envelope");
    const auto onsetError = sonare_onset_strength(
        samples.data(), samples.size(), sampleRate, 2048, kOnsetHopLength, 128,
        &onsetData, &onsetCount);
    if (logger)
        logger(std::format(
            "libsonare raw onset envelope error={} hop={} frame_count={}",
            static_cast<int>(onsetError), kOnsetHopLength, onsetCount));
    if (logger && onsetError == SONARE_OK) {
        for (size_t index = 0; index < onsetCount; ++index) {
            logger(std::format(
                "libsonare raw onset frame={} time={:.6f}s strength={:.9f}",
                index, index * static_cast<double>(kOnsetHopLength) / sampleRate,
                onsetData[index]));
        }
    }
    if (beats.empty() || onsetError != SONARE_OK) {
        sonare_free_floats(onsetData);
        return {};
    }

    const OnsetEnvelope onset{
        {onsetData, onsetCount}, static_cast<double>(sampleRate) / kOnsetHopLength};
    const auto strengths = beatStrengths(beats, onset);
    auto result = estimateMeterMap(beats, strengths, logger);
    sonare_free_floats(onsetData);
    return result;
}

} // namespace uapmd_mir::sonare
