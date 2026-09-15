#pragma once

#include <functional>
#include <span>
#include <vector>

#include "DrumScriptClassifier.hpp"
#include "NoteClipWriter.hpp"

// Turning a drum stem into notes: find the hits, measure each one, and name the
// drums that were struck.
//
// Basic Pitch finds nothing here -- it is a pitch model, and percussion has no
// pitch to find -- so a drum stem needs this backend instead. The two produce
// the same `TranscribedNote` representation and write clips through the same
// writer; only the analysis differs.
namespace uapmd_drumscript {

// DrumScript's thresholds were measured at this rate, and the spectral ones
// depend on it: n_fft = 2048 resolves 21.5 Hz per bin here, which is already
// coarse next to the 92 Hz / 120 Hz tom boundaries. Analysing at another rate
// would move every peak-frequency decision.
inline constexpr double kAnalysisSampleRate = 44100.0;

struct DrumOnset {
    double time_seconds{};
    // Peak level in this onset's window relative to the loudest in the take.
    // DrumScript computes this to gate transients and then discards it; a note
    // needs it, because drums without dynamics do not sound like drumming.
    float velocity{};
};

// Peak-picks the percussive component. `sample_rate` should be
// kAnalysisSampleRate; the caller resamples.
std::vector<DrumOnset> detectOnsets(std::span<const float> mono, double sample_rate);

// How long a drum note is held. Percussion is a one-shot -- the sound is over
// long before any sensible note-off -- so this only has to be long enough for a
// receiving sampler to latch the note.
inline constexpr double kDrumNoteSeconds = 0.05;

// The whole pipeline. One onset can yield several notes: a kick and a hi-hat
// struck together are two, and that is the case the classifier exists to get
// right. `progress` is called with a 0..1 fraction; returning false cancels and
// yields whatever was complete.
std::vector<uapmd_pitch::TranscribedNote> transcribeDrums(
    std::span<const float> mono,
    double sample_rate,
    const DrumThresholds& thresholds = {},
    const std::function<bool(double)>& progress = {});

} // namespace uapmd_drumscript
