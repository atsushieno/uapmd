#include "MirMeterMap.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <limits>
#include <numeric>
#include <ranges>

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

} // namespace

std::vector<float> beatStrengths(
        std::span<const float> beatSeconds, const OnsetEnvelope& onset) {
    std::vector<float> strengths(beatSeconds.size());
    if (onset.strengths.empty() || onset.framesPerSecond <= 0.0)
        return strengths;
    for (size_t index = 0; index < beatSeconds.size(); ++index) {
        const auto frame = static_cast<int64_t>(std::llround(
            beatSeconds[index] * onset.framesPerSecond));
        for (auto offset = -2; offset <= 2; ++offset)
            if (frame + offset >= 0
                && static_cast<size_t>(frame + offset) < onset.strengths.size())
                strengths[index] = std::max(
                    strengths[index],
                    onset.strengths[static_cast<size_t>(frame + offset)]);
    }
    if (!strengths.empty()) {
        const auto [minimum, maximum] = std::minmax_element(strengths.begin(), strengths.end());
        if (*maximum > *minimum)
            for (auto& value : strengths)
                value = (value - *minimum) / (*maximum - *minimum);
    }
    return strengths;
}

std::vector<std::tuple<double, uint8_t, uint8_t>> estimateMeterMap(
        std::span<const float> beats, std::span<const float> strengths,
        const AnalysisLogger& logger) {
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

    std::vector<std::tuple<double, uint8_t, uint8_t>> result;
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
        if (best)
            result.emplace_back(0.0, static_cast<uint8_t>(best->numerator),
                                static_cast<uint8_t>(best->denominator));
    } else {
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
        result.reserve(merged.size());
        for (const auto& segment : merged)
            result.emplace_back(segment.start_time,
                                static_cast<uint8_t>(segment.candidate.numerator),
                                static_cast<uint8_t>(segment.candidate.denominator));
    }

    if (logger)
        for (const auto& [time, numerator, denominator] : result)
            logger(std::format(
                "uapmd meter-map output time={:.6f}s signature={}/{}",
                time, numerator, denominator));
    return result;
}

} // namespace uapmd_mir
