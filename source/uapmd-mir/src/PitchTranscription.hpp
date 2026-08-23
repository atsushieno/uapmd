#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "NoteClipWriter.hpp"

namespace uapmd_pitch {

// Neither estimator segments notes: each returns one fundamental for
// one window of samples. This turns a whole mono signal into notes by running
// the estimator frame by frame, smoothing the frequency track, and cutting it
// wherever the pitch moves or the signal goes quiet. Monophonic only -- a
// chord yields whichever partial the estimator locks onto.
enum class PitchAlgorithm {
    Mpm, // McLeod Pitch Method: steadier on sustained tones
    Yin, // YIN: faster to follow a moving pitch, noisier
};

struct PitchTranscriptionOptions {
    PitchAlgorithm algorithm{PitchAlgorithm::Mpm};
    // Window the estimator sees, and how far it advances between windows.
    int frame_length{2048};
    int hop_length{512};
    // Estimates outside this range are discarded as octave errors or rumble.
    double fmin{55.0};   // A1
    double fmax{2093.0}; // C7
    // Frames quieter than this are unvoiced, and also set the floor of the
    // range that maps loudness onto velocity.
    double silence_threshold_db{-50.0};
    // Frames of context for the running median that removes single-frame
    // octave jumps. Even values are rounded up to keep the window centred.
    int median_filter_frames{5};
    // How far the pitch may drift inside one note before it becomes the next.
    double pitch_tolerance_semitones{0.8};
    // Anything shorter is treated as estimator noise rather than a note.
    double min_note_duration_seconds{0.06};
};

// `progress` is called with a 0..1 fraction as analysis advances; returning
// false cancels and yields whatever notes were complete at that point. Pass an
// empty function to run to completion.
std::vector<TranscribedNote> transcribeMonoToNotes(
    const std::vector<float>& mono,
    double sample_rate,
    const PitchTranscriptionOptions& options,
    const std::function<bool(double)>& progress = {});

} // namespace uapmd_pitch
