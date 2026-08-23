#include "PitchTranscription.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

#include "pitch_detection.h"

namespace uapmd_pitch {

namespace {

constexpr double kA4Hz = 440.0;
constexpr double kA4Note = 69.0;

double hzToSemitones(double hz) {
    return kA4Note + 12.0 * std::log2(hz / kA4Hz);
}

double amplitudeToDb(double amplitude) {
    constexpr double kFloor = 1e-9;
    return 20.0 * std::log10(std::max(amplitude, kFloor));
}

// One analysis frame. `semitones` is unset when the frame is unvoiced, either
// because it was too quiet or because the estimator declined to name a pitch.
struct Frame {
    std::optional<double> semitones;
    double db{-std::numeric_limits<double>::infinity()};
};

double rms(const std::vector<float>& window) {
    double sum = 0.0;
    for (const auto sample : window)
        sum += static_cast<double>(sample) * static_cast<double>(sample);
    return window.empty() ? 0.0 : std::sqrt(sum / static_cast<double>(window.size()));
}

// Replaces each frame's pitch with the median of the voiced pitches around it.
// A single frame that jumped an octave is outvoted by its neighbours; a run of
// frames where most neighbours are unvoiced is dropped entirely, which trims
// the ragged edges of a note rather than letting them start it early.
std::vector<Frame> medianFilter(const std::vector<Frame>& frames, int windowFrames) {
    if (windowFrames <= 1)
        return frames;
    const int radius = windowFrames / 2;
    std::vector<Frame> filtered = frames;
    std::vector<double> neighbourhood;
    neighbourhood.reserve(static_cast<size_t>(radius) * 2 + 1);

    for (size_t index = 0; index < frames.size(); ++index) {
        neighbourhood.clear();
        const auto first = static_cast<int>(index) - radius;
        const auto last = static_cast<int>(index) + radius;
        for (int neighbour = first; neighbour <= last; ++neighbour) {
            if (neighbour < 0 || neighbour >= static_cast<int>(frames.size()))
                continue;
            if (const auto& value = frames[static_cast<size_t>(neighbour)].semitones)
                neighbourhood.push_back(*value);
        }
        const auto considered = std::min(last, static_cast<int>(frames.size()) - 1)
            - std::max(first, 0) + 1;
        if (static_cast<int>(neighbourhood.size()) * 2 <= considered) {
            filtered[index].semitones.reset();
            continue;
        }
        const auto middle = neighbourhood.begin()
            + static_cast<std::ptrdiff_t>(neighbourhood.size() / 2);
        std::nth_element(neighbourhood.begin(), middle, neighbourhood.end());
        filtered[index].semitones = *middle;
    }
    return filtered;
}

double median(std::vector<double> values) {
    if (values.empty())
        return 0.0;
    const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
    std::nth_element(values.begin(), middle, values.end());
    return *middle;
}

// A note under construction: the frames accepted into it so far.
struct NoteAccumulator {
    size_t first_frame{};
    size_t last_frame{};
    std::vector<double> semitones;
    double peak_db{-std::numeric_limits<double>::infinity()};
};

float velocityFromDb(double peakDb, double silenceThresholdDb) {
    const auto range = -silenceThresholdDb;
    if (range <= 0.0)
        return 1.0f;
    const auto normalized = (peakDb - silenceThresholdDb) / range;
    return static_cast<float>(std::clamp(normalized, 0.05, 1.0));
}

} // namespace

std::vector<TranscribedNote> transcribeMonoToNotes(
        const std::vector<float>& mono,
        double sample_rate,
        const PitchTranscriptionOptions& options,
        const std::function<bool(double)>& progress) {
    std::vector<TranscribedNote> notes;
    const auto frameLength = std::max(64, options.frame_length);
    const auto hopLength = std::max(1, options.hop_length);
    if (sample_rate <= 0.0 || mono.size() < static_cast<size_t>(frameLength))
        return notes;

    const auto frameCount =
        (mono.size() - static_cast<size_t>(frameLength)) / static_cast<size_t>(hopLength) + 1;

    // The pitch_alloc classes exist so the FFT scratch space is allocated once
    // for a fixed window size and reused across every frame.
    pitch_alloc::Mpm<float> mpm{frameLength};
    pitch_alloc::Yin<float> yin{frameLength};

    std::vector<Frame> frames;
    frames.reserve(frameCount);
    std::vector<float> window(static_cast<size_t>(frameLength));

    for (size_t frameIndex = 0; frameIndex < frameCount; ++frameIndex) {
        if (progress && !progress(static_cast<double>(frameIndex) / static_cast<double>(frameCount)))
            break;

        const auto offset = frameIndex * static_cast<size_t>(hopLength);
        std::copy_n(mono.begin() + static_cast<std::ptrdiff_t>(offset), frameLength, window.begin());

        Frame frame;
        frame.db = amplitudeToDb(rms(window));
        if (frame.db >= options.silence_threshold_db) {
            const auto hz = options.algorithm == PitchAlgorithm::Yin
                ? yin.pitch(window, static_cast<int>(sample_rate))
                : mpm.pitch(window, static_cast<int>(sample_rate));
            if (hz >= options.fmin && hz <= options.fmax)
                frame.semitones = hzToSemitones(static_cast<double>(hz));
        }
        frames.push_back(frame);
    }

    const auto windowFrames = options.median_filter_frames <= 1
        ? 1
        : options.median_filter_frames | 1;
    frames = medianFilter(frames, windowFrames);

    // A frame's estimate describes the whole window, so it is dated to the
    // window's centre rather than its leading edge -- timestamping by the edge
    // drags every onset earlier by half a window. Note boundaries then fall
    // half a hop outside the first and last centres, which is what makes
    // adjacent notes meet instead of overlapping.
    const auto centreSecondsOf = [&](size_t frameIndex) {
        return static_cast<double>(frameIndex * static_cast<size_t>(hopLength)
                                   + static_cast<size_t>(frameLength) / 2) / sample_rate;
    };
    const auto halfHopSeconds = static_cast<double>(hopLength) / 2.0 / sample_rate;

    std::optional<NoteAccumulator> current;
    const auto flush = [&] {
        if (!current)
            return;
        const auto start = std::max(0.0, centreSecondsOf(current->first_frame) - halfHopSeconds);
        const auto end = centreSecondsOf(current->last_frame) + halfHopSeconds;
        if (end - start >= options.min_note_duration_seconds) {
            const auto semitones = median(current->semitones);
            TranscribedNote note;
            note.start_seconds = start;
            note.end_seconds = end;
            note.detected_semitones = semitones;
            note.note_number = static_cast<uint8_t>(
                std::clamp(static_cast<int>(std::lround(semitones)), 0, 127));
            note.velocity = velocityFromDb(current->peak_db, options.silence_threshold_db);
            notes.push_back(note);
        }
        current.reset();
    };

    for (size_t frameIndex = 0; frameIndex < frames.size(); ++frameIndex) {
        const auto& frame = frames[frameIndex];
        if (!frame.semitones) {
            flush();
            continue;
        }
        // Compare against the running median rather than the previous frame, so
        // that a slow glide stays one note while a step change starts another.
        if (current
            && std::abs(*frame.semitones - median(current->semitones))
                > options.pitch_tolerance_semitones)
            flush();
        if (!current)
            current = NoteAccumulator{frameIndex, frameIndex, {}, frame.db};
        current->last_frame = frameIndex;
        current->semitones.push_back(*frame.semitones);
        current->peak_db = std::max(current->peak_db, frame.db);
    }
    flush();

    return notes;
}

} // namespace uapmd_pitch
