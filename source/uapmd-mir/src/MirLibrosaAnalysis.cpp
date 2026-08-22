#include "MirLibrosaAnalysis.hpp"
#include "MirMeterMap.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <limits>
#include <optional>

#include <librosa/beat.hpp>
#include <librosa/feature/spectral.hpp>
#include <librosa/onset.hpp>

namespace uapmd_mir::librosa_cpp {
namespace {

constexpr int kHopLength = 512;
constexpr int kOnsetFftSize = 2048;
constexpr int kChromaFftSize = 4096;

// Tempo search band, matching what the libsonare backend asks for.
constexpr double kMinimumBpm = 40.0;
constexpr double kMaximumBpm = 240.0;
// Width of the log-normal tempo prior in octaves; librosa's own std_bpm
// default. The prior is centred on the caller's fallback tempo.
constexpr double kTempoPriorOctaves = 1.0;
constexpr size_t kMaximumTempoCandidates = 10;
// How close to the best peak a candidate has to score before it counts as tied
// with it for the purpose of resolving an octave error.
constexpr double kOctaveTieScoreRatio = 0.9;
constexpr double kBeatTrackTightness = 100.0;

// Chord post-processing, mirroring the libsonare chord detector's options.
constexpr double kChordSmoothingSeconds = 0.5;
constexpr double kChordMinimumSeconds = 0.25;

double onsetFramesPerSecond(int sampleRate) {
    return static_cast<double>(sampleRate) / kHopLength;
}

librosa::ArrayXr toArray(std::span<const float> samples) {
    librosa::ArrayXr result(static_cast<Eigen::Index>(samples.size()));
    for (size_t index = 0; index < samples.size(); ++index)
        result(static_cast<Eigen::Index>(index)) = samples[index];
    return result;
}

// The shared tempo/meter code works on float envelopes so both backends can
// feed it; librosa computes in double.
std::vector<float> toFloatEnvelope(const librosa::ArrayXr& onset) {
    std::vector<float> result(static_cast<size_t>(onset.size()));
    for (Eigen::Index index = 0; index < onset.size(); ++index)
        result[static_cast<size_t>(index)] = static_cast<float>(onset(index));
    return result;
}

bool octaveRelated(double left, double right) {
    const auto ratio = std::max(left, right) / std::min(left, right);
    return std::abs(ratio - 1.0) < 0.05 || std::abs(ratio - 2.0) < 0.15;
}

// libsonare does not report its best-scoring candidate as the window's BPM: for
// accented material the strongest autocorrelation peak is regularly the half-
// or double-tempo harmonic, and its candidate list shows the same inversion
// this one does. Resolve it the same way -- among candidates that score close
// enough to the best to be a tie, take the one nearest the prior tempo. The
// prior is already in the score, but log1p compression flattens the peaks too
// much for it to decide on its own.
size_t selectPrimaryCandidate(
        const std::vector<std::pair<double, double>>& candidates, double priorBpm) {
    size_t best = 0;
    auto bestDistance = std::abs(std::log2(candidates.front().first / priorBpm));
    for (size_t index = 1; index < candidates.size(); ++index) {
        if (candidates[index].second < kOctaveTieScoreRatio)
            continue;
        const auto distance = std::abs(std::log2(candidates[index].first / priorBpm));
        if (distance < bestDistance) {
            best = index;
            bestDistance = distance;
        }
    }
    return best;
}

// How strongly the window commits to its tempo. Octave-related candidates are
// ignored: selectPrimaryCandidate has already settled those, and reporting them
// as doubt would let the shared tempo map's tail-half rule flip the very octave
// decision that was just made. What is left is a rival only when it scores as
// well as the primary; a peak at a third of the score is not an alternative
// hypothesis.
double windowConfidence(
        const std::vector<std::pair<double, double>>& candidates, double primaryBpm) {
    double rival = 0.0;
    for (const auto& [bpm, score] : candidates)
        if (!octaveRelated(bpm, primaryBpm))
            rival = std::max(rival, score);
    return rival >= kOctaveTieScoreRatio ? 1.0 - rival : 1.0;
}

// Estimates the tempo of one analysis window, reproducing the objective
// librosa::beat::tempo() maximizes -- log1p-compressed autocorrelation plus a
// log-normal prior -- because tempo() returns only the argmax, while the shared
// tempo map also needs the runner-up peaks to resolve octave errors.
std::optional<TempoWindowEstimate> analyzeTempoWindow(
        const librosa::ArrayXr& onset, int sampleRate, double startSeconds,
        double endSeconds, double priorBpm) {
    const auto framesPerSecond = onsetFramesPerSecond(sampleRate);
    const auto firstFrame = std::max<Eigen::Index>(0, static_cast<Eigen::Index>(
        std::llround(startSeconds * framesPerSecond)));
    const auto lastFrame = std::min<Eigen::Index>(onset.size(), static_cast<Eigen::Index>(
        std::llround(endSeconds * framesPerSecond)));
    const auto frameCount = lastFrame - firstFrame;
    if (frameCount < 4)
        return std::nullopt;

    // One autocorrelation over the whole window: the analysis window *is* the
    // correlation window, so there is nothing to average over and no ramp
    // padding inside it.
    const librosa::ArrayXr window = onset.segment(firstFrame, frameCount);
    const auto tempogram = librosa::beat::tempogram(
        window, sampleRate, kHopLength, static_cast<int>(frameCount), false);
    if (tempogram.cols() < 1 || tempogram.rows() < 3)
        return std::nullopt;
    const librosa::ArrayXr correlation = tempogram.col(0);

    const auto bpmOfLag = [framesPerSecond](double lag) {
        return 60.0 * framesPerSecond / lag;
    };

    std::vector<double> scores(static_cast<size_t>(correlation.size()),
                               -std::numeric_limits<double>::infinity());
    for (Eigen::Index lag = 1; lag < correlation.size(); ++lag) {
        const auto bpm = bpmOfLag(static_cast<double>(lag));
        if (bpm < kMinimumBpm || bpm > kMaximumBpm)
            continue;
        const auto logRatio = (std::log2(bpm) - std::log2(priorBpm)) / kTempoPriorOctaves;
        scores[static_cast<size_t>(lag)] =
            std::log1p(1.0e6 * std::max(0.0, correlation(lag))) - 0.5 * logRatio * logRatio;
    }

    // Peaks only: adjacent lags describe the same tempo, and every rule that
    // reads the candidate list compares genuinely different tempi.
    std::vector<std::pair<double, double>> candidates;
    for (size_t lag = 1; lag + 1 < scores.size(); ++lag) {
        if (!std::isfinite(scores[lag]))
            continue;
        if (scores[lag] < scores[lag - 1] || scores[lag] < scores[lag + 1])
            continue;
        // Parabolic interpolation over the neighbouring lags, so the reported
        // BPM is not quantized to the autocorrelation lag grid. Without it the
        // spacing near 120 BPM is several BPM wide, and a tempo map built from
        // those values drifts against the audio within a minute.
        auto refinedLag = static_cast<double>(lag);
        if (std::isfinite(scores[lag - 1]) && std::isfinite(scores[lag + 1])) {
            const auto curvature = scores[lag - 1] - 2.0 * scores[lag] + scores[lag + 1];
            if (std::abs(curvature) > 1.0e-12)
                refinedLag += std::clamp(
                    0.5 * (scores[lag - 1] - scores[lag + 1]) / curvature, -0.5, 0.5);
        }
        candidates.emplace_back(bpmOfLag(refinedLag), scores[lag]);
    }
    if (candidates.empty())
        return std::nullopt;

    std::ranges::sort(candidates, [](const auto& left, const auto& right) {
        return left.second > right.second;
    });
    if (candidates.size() > kMaximumTempoCandidates)
        candidates.resize(kMaximumTempoCandidates);

    // Scores are log-domain; express them relative to the best peak so the
    // ratio tests in the shared tempo map read the same as libsonare's
    // confidences do.
    const auto bestScore = candidates.front().second;
    for (auto& [bpm, score] : candidates)
        score = std::exp(score - bestScore);

    const auto primaryBpm = candidates[selectPrimaryCandidate(candidates, priorBpm)].first;
    const auto confidence = windowConfidence(candidates, primaryBpm);
    return TempoWindowEstimate{primaryBpm, confidence, std::move(candidates)};
}

// librosa's beat tracker can lock on well into the file. The meter estimator
// treats the first beat of its opening segment as a downbeat at t=0, so extend
// the grid backwards at the opening tempo rather than letting the bar phase
// start wherever tracking happened to begin.
void restoreLeadingBeats(std::vector<float>& beats, double bpm, double framesPerSecond) {
    if (beats.empty() || !std::isfinite(bpm) || bpm <= 0.0 || framesPerSecond <= 0.0)
        return;
    const auto period = 60.0 / bpm;
    const auto tolerance = 0.5 / framesPerSecond;
    std::vector<float> leading;
    auto time = static_cast<double>(beats.front());
    while (time - period >= -tolerance) {
        time -= period;
        leading.push_back(static_cast<float>(std::max(0.0, time)));
    }
    if (leading.empty())
        return;
    std::ranges::reverse(leading);
    beats.insert(beats.begin(), leading.begin(), leading.end());
}

std::vector<float> detectTempoAwareBeats(
        const librosa::ArrayXr& onset, int sampleRate,
        const std::vector<std::pair<double, double>>& tempoPoints,
        double durationSeconds, const AnalysisLogger& logger) {
    const auto framesPerSecond = onsetFramesPerSecond(sampleRate);
    std::vector<float> result;
    for (size_t segment = 0; segment < tempoPoints.size(); ++segment) {
        const auto start = std::max(0.0, tempoPoints[segment].first);
        const auto end = segment + 1 < tempoPoints.size()
            ? std::min(durationSeconds, tempoPoints[segment + 1].first) : durationSeconds;
        if (end <= start)
            continue;
        const auto firstFrame = std::max<Eigen::Index>(0, static_cast<Eigen::Index>(
            std::llround(start * framesPerSecond)));
        const auto lastFrame = std::min<Eigen::Index>(onset.size(), static_cast<Eigen::Index>(
            std::llround(end * framesPerSecond)));
        if (lastFrame - firstFrame < 2)
            continue;

        if (logger)
            logger(std::format(
                "uapmd calling librosa.cpp beats section_start={:.6f}s section_end={:.6f}s",
                start, end));
        const librosa::ArrayXr section = onset.segment(firstFrame, lastFrame - firstFrame);
        const auto [sectionBpm, frames] = librosa::beat::beat_track(
            section, sampleRate, kHopLength, tempoPoints[segment].second,
            kBeatTrackTightness, false);
        if (logger)
            logger(std::format(
                "librosa.cpp raw beats section_start={:.6f}s section_end={:.6f}s bpm={:.6f} count={}",
                start, end, sectionBpm, frames.size()));
        for (size_t index = 0; index < frames.size(); ++index) {
            const auto time = static_cast<float>(
                static_cast<double>(firstFrame + frames[index]) / framesPerSecond);
            if (logger)
                logger(std::format(
                    "librosa.cpp raw beat section_start={:.6f}s index={} frame={} absolute={:.6f}s",
                    start, index, frames[index], time));
            if ((result.empty() || time > result.back() + 0.05f) && time < end)
                result.push_back(time);
        }
    }
    return result;
}

librosa::ArrayXr onsetEnvelope(const librosa::ArrayXr& mono, int sampleRate,
                               const AnalysisLogger& logger) {
    if (logger)
        logger("uapmd calling librosa.cpp onset envelope");
    auto onset = librosa::onset::onset_strength(
        mono, static_cast<double>(sampleRate), kOnsetFftSize, kHopLength);
    if (logger)
        logger(std::format(
            "librosa.cpp raw onset envelope fft={} hop={} frame_count={}",
            kOnsetFftSize, kHopLength, onset.size()));
    return onset;
}

} // namespace

std::vector<std::pair<double, double>> detectTempoMap(
        std::span<const float> samples, int sampleRate, double fallbackBpm,
        const AnalysisLogger& logger) {
    if (samples.empty() || sampleRate <= 0)
        return {{0.0, fallbackBpm}};

    const auto durationSeconds = static_cast<double>(samples.size()) / sampleRate;
    const auto mono = toArray(samples);
    const auto onset = onsetEnvelope(mono, sampleRate, logger);
    const auto onsetFloats = toFloatEnvelope(onset);
    const OnsetEnvelope envelope{onsetFloats, onsetFramesPerSecond(sampleRate)};

    const TempoWindowEstimator estimateWindow =
        [&](double start, double end) -> std::optional<TempoWindowEstimate> {
        if (logger)
            logger(std::format(
                "uapmd calling librosa.cpp BPM window start={:.6f}s end={:.6f}s",
                start, end));
        auto estimate = analyzeTempoWindow(onset, sampleRate, start, end, fallbackBpm);
        if (!estimate) {
            if (logger)
                logger(std::format(
                    "librosa.cpp raw BPM window start={:.6f}s end={:.6f}s unavailable",
                    start, end));
            return std::nullopt;
        }
        if (logger) {
            logger(std::format(
                "librosa.cpp raw BPM window start={:.6f}s end={:.6f}s bpm={:.6f} confidence={:.6f}",
                start, end, estimate->bpm, estimate->confidence));
            for (size_t index = 0; index < estimate->candidates.size(); ++index)
                logger(std::format(
                    "librosa.cpp raw BPM candidate window_start={:.6f}s index={} bpm={:.6f} confidence={:.6f}",
                    start, index, estimate->candidates[index].first,
                    estimate->candidates[index].second));
        }
        return estimate;
    };

    return buildTempoMap(durationSeconds, estimateWindow, envelope, fallbackBpm, logger);
}

std::vector<std::tuple<double, uint8_t, uint8_t>> detectRhythmMap(
        std::span<const float> samples, int sampleRate,
        const std::vector<std::pair<double, double>>& tempoPoints, double fallbackBpm,
        const AnalysisLogger& logger) {
    if (samples.empty() || sampleRate <= 0)
        return {};

    const auto durationSeconds = static_cast<double>(samples.size()) / sampleRate;
    const auto mono = toArray(samples);
    const auto onset = onsetEnvelope(mono, sampleRate, logger);
    const auto onsetFloats = toFloatEnvelope(onset);
    const OnsetEnvelope envelope{onsetFloats, onsetFramesPerSecond(sampleRate)};

    auto beats = detectTempoAwareBeats(
        onset, sampleRate, tempoPoints, durationSeconds, logger);
    if (beats.empty())
        return {};
    const auto openingBpm = tempoPoints.empty() ? fallbackBpm : tempoPoints.front().second;
    restoreLeadingBeats(beats, openingBpm, envelope.framesPerSecond);

    const auto strengths = beatStrengths(beats, envelope);
    return estimateMeterMap(beats, strengths, logger);
}

std::vector<std::pair<double, std::string>> detectChords(
        std::span<const float> samples, int sampleRate, const AnalysisLogger& logger) {
    static constexpr std::array<std::string_view, 12> roots{
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    if (samples.empty() || sampleRate <= 0)
        return {};

    if (logger)
        logger("uapmd calling librosa.cpp chroma");
    const auto chroma = librosa::feature::chroma_stft(
        toArray(samples), static_cast<double>(sampleRate), kChromaFftSize, kHopLength);
    const auto frameCount = chroma.cols();
    const auto framesPerSecond = onsetFramesPerSecond(sampleRate);
    if (frameCount <= 0)
        return {};

    // Smooth before deciding, then reject short segments. A per-frame argmax
    // emits a chord change every ~12 ms whenever the chroma wobbles.
    const auto smoothingRadius = std::max<Eigen::Index>(1, static_cast<Eigen::Index>(
        std::llround(0.5 * kChordSmoothingSeconds * framesPerSecond)));
    std::vector<std::string> labels(static_cast<size_t>(frameCount));
    for (Eigen::Index frame = 0; frame < frameCount; ++frame) {
        std::array<double, 12> smoothed{};
        const auto first = std::max<Eigen::Index>(0, frame - smoothingRadius);
        const auto last = std::min(frameCount, frame + smoothingRadius + 1);
        for (auto index = first; index < last; ++index)
            for (int pitchClass = 0; pitchClass < 12; ++pitchClass)
                smoothed[static_cast<size_t>(pitchClass)] += chroma(pitchClass, index);

        auto bestScore = -std::numeric_limits<double>::infinity();
        int bestRoot = 0;
        bool minor = false;
        for (int root = 0; root < 12; ++root) {
            double majorScore = 0.0;
            double minorScore = 0.0;
            for (int interval = 0; interval < 12; ++interval) {
                const auto value = smoothed[static_cast<size_t>((root + interval) % 12)];
                majorScore += value * (interval == 0 || interval == 7 ? 1.0
                    : interval == 4 ? 0.8 : interval == 3 || interval == 8 ? 0.2 : -0.2);
                minorScore += value * (interval == 0 || interval == 7 ? 1.0
                    : interval == 3 ? 0.8 : interval == 4 || interval == 8 ? 0.2 : -0.2);
            }
            if (majorScore > bestScore) {
                bestScore = majorScore;
                bestRoot = root;
                minor = false;
            }
            if (minorScore > bestScore) {
                bestScore = minorScore;
                bestRoot = root;
                minor = true;
            }
        }
        labels[static_cast<size_t>(frame)] =
            std::format("{}{}", roots[static_cast<size_t>(bestRoot)], minor ? "m" : "");
    }

    struct ChordRun {
        size_t start;
        size_t end;
        std::string label;
    };
    std::vector<ChordRun> runs;
    for (size_t frame = 0; frame < labels.size(); ++frame) {
        if (!runs.empty() && runs.back().label == labels[frame]) {
            runs.back().end = frame + 1;
            continue;
        }
        runs.push_back({frame, frame + 1, labels[frame]});
    }

    const auto minimumFrames = std::max<size_t>(1, static_cast<size_t>(
        std::llround(kChordMinimumSeconds * framesPerSecond)));
    std::vector<ChordRun> kept;
    for (const auto& run : runs) {
        // A run too short to be a chord is absorbed by the one it interrupted;
        // the very first run has nothing to absorb it and stays as the clip's
        // opening chord.
        if (!kept.empty()
            && (run.end - run.start < minimumFrames || kept.back().label == run.label)) {
            kept.back().end = run.end;
            continue;
        }
        kept.push_back(run);
    }

    std::vector<std::pair<double, std::string>> result;
    result.reserve(kept.size());
    for (const auto& run : kept)
        result.emplace_back(static_cast<double>(run.start) / framesPerSecond, run.label);
    if (logger) {
        logger(std::format("librosa.cpp raw chords count={}", result.size()));
        for (size_t index = 0; index < result.size(); ++index)
            logger(std::format(
                "librosa.cpp raw chord index={} start={:.6f}s label={}",
                index, result[index].first, result[index].second));
    }
    return result;
}

} // namespace uapmd_mir::librosa_cpp
