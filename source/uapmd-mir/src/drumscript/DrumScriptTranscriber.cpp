#include "DrumScriptTranscriber.hpp"

#include <algorithm>
#include <cmath>

#include <librosa/effects.hpp>
#include <librosa/onset.hpp>

namespace uapmd_drumscript {

namespace {

// Onsets are picked on the same hop the spectral features use.
constexpr int kOnsetHopSamples = kSpectrumHopSamples;
// Cymbals ring unevenly enough to re-trigger the picker; this is how long it
// refuses to pick again.
constexpr double kOnsetLockoutSeconds = 0.05;
// Below this the envelope is background noise rather than a hit.
constexpr float kOnsetDelta = 0.05f;
// Isolated-hit recordings get a second pass at this spacing. Whole takes do
// not: real drumming is denser than this and the pass would eat it.
constexpr double kShortTakeSeconds = 2.0;
constexpr double kShortTakeMinGapSeconds = 0.150;
// Stop just short of the next attack so its transient does not read as this
// hit still ringing.
constexpr double kNextOnsetGuardSeconds = 0.010;
// Fewer frames than this and the decay slope is noise. Rolls faster than this
// get a slope fitted over an overlapping window rather than no slope at all.
constexpr double kMinimumDecayWindowSeconds = 0.060;

librosa::ArrayXr toArray(std::span<const float> samples) {
    librosa::ArrayXr out(static_cast<Eigen::Index>(samples.size()));
    for (size_t i = 0; i < samples.size(); ++i)
        out[static_cast<Eigen::Index>(i)] = static_cast<librosa::Real>(samples[i]);
    return out;
}

// A window of `seconds` starting at `start`, zero-padded when the take ends
// first, so every onset is measured over the same span.
std::vector<float> sliceAt(std::span<const float> mono, double start, double seconds, double sample_rate) {
    const auto begin = static_cast<size_t>(std::max(0.0, start * sample_rate));
    const auto count = static_cast<size_t>(seconds * sample_rate);
    std::vector<float> slice(count, 0.0f);
    if (begin >= mono.size())
        return slice;
    const auto available = std::min(count, mono.size() - begin);
    std::copy_n(mono.begin() + static_cast<std::ptrdiff_t>(begin), available, slice.begin());
    return slice;
}

float peakLevel(std::span<const float> samples) {
    float peak = 0.0f;
    for (float sample : samples)
        peak = std::max(peak, std::abs(sample));
    return peak;
}

} // namespace

std::vector<DrumOnset> detectOnsets(std::span<const float> mono, double sample_rate) {
    std::vector<DrumOnset> onsets;
    if (mono.empty() || sample_rate <= 0.0)
        return onsets;

    // Harmonic content leaks into a separated stem and peaks where no drum was
    // struck, so the picker sees the percussive component only.
    const auto percussive = librosa::effects::percussive(toArray(mono));
    const auto wait = static_cast<int>(kOnsetLockoutSeconds * (sample_rate / kOnsetHopSamples));

    const auto frames = librosa::onset::onset_detect(
        percussive,
        static_cast<librosa::Real>(sample_rate),
        kOnsetHopSamples,
        /*backtrack*/ false,
        librosa::onset::OnsetUnits::Frames,
        /*normalize*/ true,
        /*pre_max*/ 0, /*post_max*/ 0, /*pre_avg*/ 0, /*post_avg*/ 0,
        kOnsetDelta,
        wait);

    std::vector<double> times;
    times.reserve(frames.size());
    for (auto frame : frames)
        times.push_back(static_cast<double>(frame) * kOnsetHopSamples / sample_rate);

    const double duration = static_cast<double>(mono.size()) / sample_rate;
    if (duration < kShortTakeSeconds && times.size() > 1) {
        std::vector<double> refined{times.front()};
        for (size_t i = 1; i < times.size(); ++i) {
            if (times[i] - refined.back() > kShortTakeMinGapSeconds)
                refined.push_back(times[i]);
        }
        times = std::move(refined);
    }

    const float globalPeak = std::max(peakLevel(mono), 1e-9f);
    onsets.reserve(times.size());
    for (double time : times) {
        const auto window = sliceAt(mono, time, kShortSliceSeconds, sample_rate);
        onsets.push_back({time, std::clamp(peakLevel(window) / globalPeak, 0.0f, 1.0f)});
    }
    return onsets;
}

std::vector<uapmd_pitch::TranscribedNote> transcribeDrums(
        std::span<const float> mono,
        double sample_rate,
        const DrumThresholds& thresholds,
        const std::function<bool(double)>& progress) {
    std::vector<uapmd_pitch::TranscribedNote> notes;
    const auto onsets = detectOnsets(mono, sample_rate);
    if (onsets.empty())
        return notes;

    for (size_t index = 0; index < onsets.size(); ++index) {
        if (progress && !progress(static_cast<double>(index) / onsets.size()))
            break;

        const auto& onset = onsets[index];
        const auto shortSlice = sliceAt(mono, onset.time_seconds, kShortSliceSeconds, sample_rate);

        // Decay can only be measured until the next hit; past that the level is
        // someone else's. The classifier extrapolates from whatever window it
        // gets, so a short one costs accuracy rather than correctness.
        double decayWindow = kLongSliceSeconds;
        if (index + 1 < onsets.size()) {
            const double untilNext =
                onsets[index + 1].time_seconds - onset.time_seconds - kNextOnsetGuardSeconds;
            decayWindow = std::clamp(untilNext, kMinimumDecayWindowSeconds, kLongSliceSeconds);
        }
        const auto longSlice = sliceAt(mono, onset.time_seconds, decayWindow, sample_rate);
        const auto physics = measure(shortSlice, longSlice, sample_rate);

        for (auto instrument : classify(physics, thresholds)) {
            const int note = generalMidiNote(instrument);
            // Unknown: the onset was real but matched no rule. Dropping it is
            // better than inventing a drum, and the onset is still audible in
            // the stem the clip sits beside.
            if (note < 0)
                continue;
            uapmd_pitch::TranscribedNote transcribed;
            transcribed.start_seconds = onset.time_seconds;
            transcribed.end_seconds = onset.time_seconds + kDrumNoteSeconds;
            transcribed.note_number = static_cast<uint8_t>(note);
            transcribed.detected_semitones = note;
            transcribed.velocity = onset.velocity;
            notes.push_back(std::move(transcribed));
        }
    }
    return notes;
}

} // namespace uapmd_drumscript
