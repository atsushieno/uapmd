#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include <uapmd-midi-service/uapmd-midi-service.hpp>

// The note representation every transcription backend produces, and the writer
// that turns it into a MIDI 2.0 clip. Shared so that a backend only has to
// decide what the notes are, not how they are spelled as UMP.
//
// Compiled into each backend rather than into a library of its own: the
// backends end up in separate binaries -- one dynamically loaded addin, one
// static built-in -- so there is nothing for a shared copy to prevent.
namespace uapmd_pitch {

// One point on a note's pitch curve, relative to the note's own start.
// Backends that only estimate a single pitch per note leave the curve empty.
struct PitchBendPoint {
    double seconds{};
    // Deviation from note_number, in semitones. +/- 2 is the range a receiver
    // is expected to assume, which is what the writer scales against.
    double semitones{};
};

struct TranscribedNote {
    double start_seconds{};
    double end_seconds{};
    // Nearest semitone, and the pitch actually detected. The two differ by
    // whatever the performance was off by, which MIDI 2.0 can carry as a
    // Pitch 7.9 note attribute rather than discard.
    uint8_t note_number{};
    double detected_semitones{};
    float velocity{}; // 0..1
    // Optional per-note pitch movement over the note's life. A backend that
    // tracks pitch continuously fills this; the writer then emits per-note
    // pitch bend alongside the note, which is expression MIDI 1.0 cannot carry
    // per note at all.
    std::vector<PitchBendPoint> bend;
};

// Builds a MIDI 2.0 clip: parallel arrays of UMP words and their absolute tick
// timestamps, ordered by tick, as the clip APIs expect. Notes may overlap.
std::pair<std::vector<uapmd_ump_t>, std::vector<uint64_t>> makeNoteClip(
    const std::vector<TranscribedNote>& notes,
    uint32_t tick_resolution,
    double bpm);

} // namespace uapmd_pitch
