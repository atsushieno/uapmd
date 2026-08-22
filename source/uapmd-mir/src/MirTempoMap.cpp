#include "MirTempoMap.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <numeric>
#include <vector>

namespace uapmd_mir {
namespace {

constexpr double kSameTempoRelativeTolerance = 0.05;

struct TempoObservation {
    double window_start{};
    double window_end{};
    double center{};
    double primary_bpm{};
    double selected_bpm{};
    bool selected_tail_half{};
    std::vector<std::pair<double, double>> candidates;
};

bool sameTempo(double left, double right) {
    return std::abs(left - right) / std::max({left, right, 1.0})
        <= kSameTempoRelativeTolerance;
}

double medianSelectedBpm(const std::vector<TempoObservation>& observations,
                         size_t first, size_t last) {
    std::vector<double> values;
    values.reserve(last - first);
    for (auto index = first; index < last; ++index)
        values.push_back(observations[index].selected_bpm);
    std::ranges::sort(values);
    return values[values.size() / 2];
}

void selectTailHalfTempo(TempoObservation& observation, double duration,
                         double analysisConfidence) {
    // A fading final window often makes the strongest autocorrelation peak the
    // double-tempo harmonic. Only resolve that ambiguity when the window reaches
    // the end, confidence is low, and the backend reports a nearly tied
    // half-tempo candidate. The selected BPM remains the exact candidate value.
    if (observation.window_end < duration - 2.0 || analysisConfidence >= 0.6
        || observation.candidates.empty())
        return;

    const auto primaryScore = observation.candidates.front().second;
    for (const auto& [candidateBpm, candidateScore] : observation.candidates) {
        if (candidateBpm < 40.0 || candidateScore < primaryScore * 0.85)
            continue;
        const auto ratio = observation.primary_bpm / candidateBpm;
        if (ratio >= 1.9 && ratio <= 2.1) {
            observation.selected_bpm = candidateBpm;
            observation.selected_tail_half = true;
            return;
        }
    }
}

void resolveShortAmbiguousRuns(std::vector<TempoObservation>& observations) {
    if (observations.size() < 3)
        return;

    size_t runStart = 1;
    while (runStart + 1 < observations.size()) {
        size_t runEnd = runStart + 1;
        while (runEnd < observations.size()
               && sameTempo(observations[runEnd].selected_bpm,
                            observations[runStart].selected_bpm))
            ++runEnd;
        if (runEnd == observations.size())
            break;

        const auto runLength = runEnd - runStart;
        if (runLength <= 3) {
            const auto previousBpm = observations[runStart - 1].selected_bpm;
            const auto nextBpm = observations[runEnd].selected_bpm;
            for (auto index = runStart; index < runEnd; ++index) {
                auto& observation = observations[index];
                if (observation.selected_tail_half)
                    continue;
                const auto bestScore = observation.candidates.empty()
                    ? 0.0 : observation.candidates.front().second;
                double replacement = observation.selected_bpm;
                double replacementDistance = std::numeric_limits<double>::infinity();
                for (const auto& [candidateBpm, candidateScore] : observation.candidates) {
                    if (bestScore > 0.0 && candidateScore < bestScore * 0.75)
                        continue;
                    const auto distance = std::min(
                        std::abs(std::log(candidateBpm / previousBpm)),
                        std::abs(std::log(candidateBpm / nextBpm)));
                    if (distance < replacementDistance) {
                        replacement = candidateBpm;
                        replacementDistance = distance;
                    }
                }
                if (sameTempo(replacement, previousBpm)
                    || sameTempo(replacement, nextBpm))
                    observation.selected_bpm = replacement;
            }
        }
        runStart = runEnd;
    }
}

double localPeriodicity(std::span<const float> onset, int center, int radius, int lag) {
    const auto first = std::max(0, center - radius);
    const auto last = std::min(static_cast<int>(onset.size()), center + radius);
    if (lag <= 0 || last - first <= lag * 2)
        return 0.0;
    double mean = 0.0;
    for (auto index = first; index < last; ++index)
        mean += onset[static_cast<size_t>(index)];
    mean /= last - first;
    double product = 0.0;
    double leftPower = 0.0;
    double rightPower = 0.0;
    for (auto index = first + lag; index < last; ++index) {
        const auto left = onset[static_cast<size_t>(index)] - mean;
        const auto right = onset[static_cast<size_t>(index - lag)] - mean;
        product += left * right;
        leftPower += left * left;
        rightPower += right * right;
    }
    const auto denominator = std::sqrt(leftPower * rightPower);
    return denominator > 1.0e-12 ? product / denominator : 0.0;
}

double refineTempoTransition(const OnsetEnvelope& onset, double coarseTime,
                             double previousBpm, double nextBpm) {
    const auto framesPerSecond = onset.framesPerSecond;
    if (onset.strengths.empty() || framesPerSecond <= 0.0)
        return coarseTime;
    const auto first = std::max(1, static_cast<int>(std::llround(
        (coarseTime - 6.0) * framesPerSecond)));
    const auto last = std::min(static_cast<int>(onset.strengths.size()) - 1,
        static_cast<int>(std::llround((coarseTime + 6.0) * framesPerSecond)));
    if (first >= last)
        return coarseTime;

    const auto previousLag = std::max(1, static_cast<int>(std::llround(
        60.0 * framesPerSecond / previousBpm)));
    const auto nextLag = std::max(1, static_cast<int>(std::llround(
        60.0 * framesPerSecond / nextBpm)));
    const auto radius = std::max(1, static_cast<int>(std::llround(2.0 * framesPerSecond)));
    std::vector<double> differences;
    differences.reserve(static_cast<size_t>(last - first + 1));
    for (auto frame = first; frame <= last; ++frame)
        differences.push_back(
            localPeriodicity(onset.strengths, frame, radius, nextLag)
            - localPeriodicity(onset.strengths, frame, radius, previousLag));

    const auto smoothingRadius = std::max(1, static_cast<int>(
        std::llround(0.25 * framesPerSecond)));
    std::vector<double> smoothed(differences.size());
    for (size_t index = 0; index < differences.size(); ++index) {
        const auto begin = index > static_cast<size_t>(smoothingRadius)
            ? index - smoothingRadius : 0;
        const auto end = std::min(differences.size(), index + smoothingRadius + 1);
        smoothed[index] = std::accumulate(
            differences.begin() + static_cast<std::ptrdiff_t>(begin),
            differences.begin() + static_cast<std::ptrdiff_t>(end), 0.0)
            / static_cast<double>(end - begin);
    }

    auto bestTime = coarseTime;
    auto bestDistance = std::numeric_limits<double>::infinity();
    for (size_t index = 1; index < smoothed.size(); ++index) {
        if (smoothed[index - 1] > 0.0 || smoothed[index] <= 0.0)
            continue;
        const auto time = static_cast<double>(first + static_cast<int>(index))
            / framesPerSecond;
        const auto distance = std::abs(time - coarseTime);
        if (distance < bestDistance) {
            bestTime = time;
            bestDistance = distance;
        }
    }
    return bestTime;
}

} // namespace

std::vector<std::pair<double, double>> buildTempoMap(
        double durationSeconds, const TempoWindowEstimator& estimateWindow,
        const OnsetEnvelope& onset, double fallbackBpm, const AnalysisLogger& logger) {
    if (!(durationSeconds > 0.0) || !estimateWindow)
        return {{0.0, fallbackBpm}};

    std::vector<TempoObservation> observations;
    for (double start = 0.0; start + kMinimumTempoWindowSeconds <= durationSeconds;
         start += kTempoWindowHopSeconds) {
        const auto end = std::min(durationSeconds, start + kTempoWindowSeconds);
        if (end <= start)
            continue;

        const auto estimate = estimateWindow(start, end);
        if (!estimate || !std::isfinite(estimate->bpm) || estimate->bpm <= 0.0)
            continue;

        TempoObservation observation{
            start, end, 0.5 * (start + end), estimate->bpm, estimate->bpm, false,
            estimate->candidates};
        selectTailHalfTempo(observation, durationSeconds, estimate->confidence);
        observations.push_back(std::move(observation));
    }

    if (observations.empty())
        return {{0.0, fallbackBpm}};

    resolveShortAmbiguousRuns(observations);

    std::vector<std::pair<size_t, size_t>> runs;
    for (size_t first = 0; first < observations.size();) {
        auto last = first + 1;
        while (last < observations.size()
               && sameTempo(observations[last].selected_bpm,
                            observations[first].selected_bpm))
            ++last;
        runs.emplace_back(first, last);
        first = last;
    }

    std::vector<std::pair<double, double>> result;
    std::vector<bool> tailHalfSelections;
    result.emplace_back(0.0, medianSelectedBpm(
        observations, runs.front().first, runs.front().second));
    tailHalfSelections.push_back(false);
    for (size_t index = 1; index < runs.size(); ++index) {
        const auto [first, last] = runs[index];
        const auto startsWithTailHalf = observations[first].selected_tail_half;
        const auto time = startsWithTailHalf
            ? observations[first].window_start
            : 0.5 * (observations[runs[index - 1].second - 1].center
                     + observations[first].center);
        result.emplace_back(time, medianSelectedBpm(observations, first, last));
        tailHalfSelections.push_back(startsWithTailHalf);
    }

    for (size_t index = 1; index < result.size(); ++index) {
        if (tailHalfSelections[index])
            continue;
        result[index].first = refineTempoTransition(
            onset, result[index].first,
            result[index - 1].second, result[index].second);
    }

    if (logger)
        for (const auto& [time, bpm] : result)
            logger(std::format(
                "uapmd tempo-map output time={:.6f}s bpm={:.6f}", time, bpm));
    return result;
}

uint64_t tempoMapTimeToTicks(
        std::span<const std::pair<double, double>> tempoPoints,
        double seconds, uint32_t tickResolution, double fallbackBpm) {
    if (!std::isfinite(seconds) || seconds <= 0.0 || tickResolution == 0)
        return 0;

    double previousTime = 0.0;
    double previousBpm = tempoPoints.empty() ? fallbackBpm : tempoPoints.front().second;
    double ticks = 0.0;
    for (const auto& [time, bpm] : tempoPoints) {
        if (time <= 0.0) {
            previousBpm = bpm;
            continue;
        }
        if (seconds <= time)
            break;
        ticks += (time - previousTime) * previousBpm / 60.0 * tickResolution;
        previousTime = time;
        previousBpm = bpm;
    }
    ticks += (seconds - previousTime) * previousBpm / 60.0 * tickResolution;
    return static_cast<uint64_t>(std::llround(std::max(0.0, ticks)));
}

} // namespace uapmd_mir
