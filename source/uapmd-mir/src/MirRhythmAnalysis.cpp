#include "MirRhythmAnalysis.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <format>
#include <numeric>
#include <ranges>

#include <sonare/sonare_c.h>

namespace uapmd_mir {
namespace {

struct MeterCandidate {
    int numerator;
    int denominator;
    std::vector<int> grouping;
};

struct MeterSegment {
    double start_time;
    MeterCandidate candidate;
};

std::vector<float> detectTempoAwareBeats(
        std::span<const float> samples, int sampleRate,
        const std::vector<std::pair<double, double>>& tempoPoints,
        const RhythmAnalysisLogger& logger) {
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

std::vector<MeterCandidate> meterCandidates() {
    static constexpr std::array<int, 10> numerators{3, 4, 5, 6, 7, 8, 9, 11, 12, 13};
    static const std::array<std::vector<std::vector<int>>, 14> groupings{
        std::vector<std::vector<int>>{},
        std::vector<std::vector<int>>{},
        std::vector<std::vector<int>>{{2}},
        std::vector<std::vector<int>>{{3}},
        std::vector<std::vector<int>>{{4}, {2, 2}},
        std::vector<std::vector<int>>{{2, 3}, {3, 2}},
        std::vector<std::vector<int>>{{3, 3}, {2, 2, 2}},
        std::vector<std::vector<int>>{{3, 2, 2}, {2, 3, 2}, {2, 2, 3}},
        std::vector<std::vector<int>>{{4, 4}, {3, 3, 2}, {3, 2, 3}, {2, 3, 3}},
        std::vector<std::vector<int>>{{3, 3, 3}, {2, 2, 2, 3}, {2, 2, 3, 2},
                                      {2, 3, 2, 2}, {3, 2, 2, 2}},
        std::vector<std::vector<int>>{{3, 3, 2, 2}, {3, 2, 3, 2}, {2, 3, 3, 2},
                                      {2, 2, 3, 3}},
        std::vector<std::vector<int>>{{3, 3, 3, 2}, {3, 3, 2, 3}, {3, 2, 3, 3},
                                      {2, 3, 3, 3}},
        std::vector<std::vector<int>>{{3, 3, 3, 3}, {4, 4, 4}},
        std::vector<std::vector<int>>{{3, 3, 3, 2, 2}, {3, 3, 2, 3, 2},
                                      {3, 2, 3, 3, 2}, {2, 3, 3, 3, 2}},
    };
    std::vector<MeterCandidate> result;
    for (const auto numerator : numerators) {
        const auto denominator = numerator >= 5 && numerator != 12 ? 8 : 4;
        for (const auto& grouping : groupings[static_cast<size_t>(numerator)])
            result.push_back({numerator, denominator, grouping});
    }
    return result;
}

double mean(std::span<const float> values) {
    if (values.empty())
        return 0.0;
    return std::accumulate(values.begin(), values.end(), 0.0) / values.size();
}

double scoreCandidate(std::span<const float> strengths, std::span<const float> beats,
                      size_t start, size_t end, const MeterCandidate& candidate) {
    std::vector<float> downbeat;
    std::vector<float> secondary;
    std::vector<float> weak;
    std::vector<int> groupStarts{0};
    int cursor = 0;
    for (size_t index = 0; index + 1 < candidate.grouping.size(); ++index) {
        cursor += candidate.grouping[index];
        groupStarts.push_back(cursor);
    }
    for (size_t index = start; index < end; ++index) {
        const auto position = static_cast<int>((index - start) % candidate.numerator);
        if (position == 0)
            downbeat.push_back(strengths[index]);
        else if (std::ranges::find(groupStarts, position) != groupStarts.end())
            secondary.push_back(strengths[index]);
        else
            weak.push_back(strengths[index]);
    }
    const auto weakMean = mean(weak);
    auto score = std::max(0.0, mean(downbeat) - weakMean)
        + 0.55 * std::max(0.0, mean(secondary) - weakMean);

    std::vector<std::vector<float>> bars;
    for (size_t index = start; index + static_cast<size_t>(candidate.numerator) <= end;
         index += candidate.numerator)
        bars.emplace_back(strengths.begin() + static_cast<std::ptrdiff_t>(index),
                          strengths.begin() + static_cast<std::ptrdiff_t>(
                              index + candidate.numerator));
    if (bars.size() >= 2) {
        std::vector<float> pattern(static_cast<size_t>(candidate.numerator));
        for (const auto& bar : bars)
            for (size_t position = 0; position < pattern.size(); ++position)
                pattern[position] += bar[position] / bars.size();
        double error = 0.0;
        for (const auto& bar : bars)
            for (size_t position = 0; position < pattern.size(); ++position)
                error += std::abs(bar[position] - pattern[position]);
        score += 0.35 * std::max(0.0, 1.0 - error / (bars.size() * pattern.size()));
    }

    std::vector<float> intervals;
    for (size_t index = start + 1; index < end; ++index)
        if (beats[index] > beats[index - 1])
            intervals.push_back(beats[index] - beats[index - 1]);
    if (intervals.size() >= 2) {
        const auto intervalMean = mean(intervals);
        double variance = 0.0;
        for (const auto interval : intervals)
            variance += (interval - intervalMean) * (interval - intervalMean);
        score += 0.25 * std::max(
            0.0, 1.0 - std::sqrt(variance / intervals.size()) / intervalMean);
    }
    return score;
}

std::vector<std::tuple<double, uint8_t, uint8_t>> estimateMeterMap(
        std::span<const float> beats, std::span<const float> strengths) {
    const auto count = std::min(beats.size(), strengths.size());
    if (count < 4)
        return {};
    const auto candidates = meterCandidates();
    constexpr size_t maxSegmentBeats = 96;
    constexpr double transitionPenalty = 1.15;
    std::vector<double> dp(count + 1, -std::numeric_limits<double>::infinity());
    std::vector<int> previous(count + 1, -1);
    std::vector<int> selected(count + 1, -1);
    dp[0] = 0.0;
    for (size_t end = 1; end <= count; ++end) {
        const auto first = end > maxSegmentBeats ? end - maxSegmentBeats : 0;
        for (size_t start = first; start < end; ++start) {
            if (!std::isfinite(dp[start]))
                continue;
            for (size_t candidateIndex = 0; candidateIndex < candidates.size();
                 ++candidateIndex) {
                const auto& candidate = candidates[candidateIndex];
                const auto length = end - start;
                if (length < static_cast<size_t>(candidate.numerator * 2)
                    || length % static_cast<size_t>(candidate.numerator) != 0)
                    continue;
                const auto objective = dp[start]
                    + scoreCandidate(strengths, beats, start, end, candidate) * length
                    - (start == 0 ? 0.0 : transitionPenalty);
                if (objective > dp[end]) {
                    dp[end] = objective;
                    previous[end] = static_cast<int>(start);
                    selected[end] = static_cast<int>(candidateIndex);
                }
            }
        }
    }
    if (previous[count] < 0) {
        double bestScore = -std::numeric_limits<double>::infinity();
        const MeterCandidate* best = nullptr;
        for (const auto& candidate : candidates) {
            if (count < static_cast<size_t>(candidate.numerator * 2)
                || count % static_cast<size_t>(candidate.numerator) != 0)
                continue;
            const auto score = scoreCandidate(strengths, beats, 0, count, candidate);
            if (score > bestScore) {
                bestScore = score;
                best = &candidate;
            }
        }
        if (!best)
            return {};
        return {{0.0, static_cast<uint8_t>(best->numerator),
                 static_cast<uint8_t>(best->denominator)}};
    }

    std::vector<MeterSegment> reversed;
    for (auto end = count; end > 0;) {
        const auto start = static_cast<size_t>(previous[end]);
        const auto& candidate = candidates[static_cast<size_t>(selected[end])];
        reversed.push_back({start == 0 ? 0.0 : beats[start], candidate});
        end = start;
    }
    std::ranges::reverse(reversed);
    std::vector<MeterSegment> merged;
    for (const auto& segment : reversed) {
        if (!merged.empty()
            && merged.back().candidate.numerator == segment.candidate.numerator
            && merged.back().candidate.denominator == segment.candidate.denominator
            && merged.back().candidate.grouping == segment.candidate.grouping)
            continue;
        merged.push_back(segment);
    }
    std::vector<std::tuple<double, uint8_t, uint8_t>> result;
    result.reserve(merged.size());
    for (const auto& segment : merged)
        result.emplace_back(segment.start_time,
                            static_cast<uint8_t>(segment.candidate.numerator),
                            static_cast<uint8_t>(segment.candidate.denominator));
    return result;
}

} // namespace

std::vector<std::tuple<double, uint8_t, uint8_t>> detectRhythmMap(
        std::span<const float> samples, int sampleRate,
        const std::vector<std::pair<double, double>>& tempoPoints, double,
        const RhythmAnalysisLogger& logger) {
    constexpr int hopLength = 512;
    const auto beats = detectTempoAwareBeats(samples, sampleRate, tempoPoints, logger);
    float* onsetData = nullptr;
    size_t onsetCount = 0;
    if (logger)
        logger("uapmd calling libsonare onset envelope");
    const auto onsetError = sonare_onset_strength(
        samples.data(), samples.size(), sampleRate, 2048, hopLength, 128,
        &onsetData, &onsetCount);
    if (logger)
        logger(std::format(
            "libsonare raw onset envelope error={} hop={} frame_count={}",
            static_cast<int>(onsetError), hopLength, onsetCount));
    if (logger && onsetError == SONARE_OK) {
        for (size_t index = 0; index < onsetCount; ++index) {
            logger(std::format(
                "libsonare raw onset frame={} time={:.6f}s strength={:.9f}",
                index, index * static_cast<double>(hopLength) / sampleRate,
                onsetData[index]));
        }
    }
    if (beats.empty() || onsetError != SONARE_OK) {
        sonare_free_floats(onsetData);
        return {};
    }

    std::vector<float> strengths(beats.size());
    for (size_t index = 0; index < beats.size(); ++index) {
        const auto frame = static_cast<int64_t>(std::llround(
            beats[index] * sampleRate / static_cast<double>(hopLength)));
        for (auto offset = -2; offset <= 2; ++offset)
            if (frame + offset >= 0 && static_cast<size_t>(frame + offset) < onsetCount)
                strengths[index] = std::max(
                    strengths[index], onsetData[static_cast<size_t>(frame + offset)]);
    }
    if (!strengths.empty()) {
        const auto [minimum, maximum] = std::minmax_element(strengths.begin(), strengths.end());
        if (*maximum > *minimum)
            for (auto& value : strengths)
                value = (value - *minimum) / (*maximum - *minimum);
    }
    const auto result = estimateMeterMap(beats, strengths);
    if (logger)
        for (const auto& [time, numerator, denominator] : result)
            logger(std::format(
                "uapmd meter-map output time={:.6f}s signature={}/{}",
                time, numerator, denominator));
    sonare_free_floats(onsetData);
    return result;
}

} // namespace uapmd_mir
