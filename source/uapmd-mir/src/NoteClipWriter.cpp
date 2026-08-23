#include "NoteClipWriter.hpp"

#include <algorithm>
#include <cmath>

#include <umppi/umppi.hpp>

namespace uapmd_pitch {

namespace {

constexpr uint8_t kGroup = 0;
constexpr uint8_t kChannel = 0;
// MIDI 2.0 note attribute type 3 carries the note's exact pitch as a 7.9 fixed
// point value, which is how a transcription keeps the cents it measured
// instead of rounding them away into the nearest semitone.
constexpr uint8_t kPitch7_9AttributeType = 3;
// Per-note pitch bend is centred at half of the 32-bit range, and a receiver is
// expected to read full deflection as two semitones.
constexpr double kBendCentre = 2147483648.0; // 0x80000000
constexpr double kBendSemitoneRange = 2.0;

uint64_t secondsToTicks(double seconds, uint32_t tickResolution, double bpm) {
    if (bpm <= 0.0 || tickResolution == 0 || seconds <= 0.0)
        return 0;
    return static_cast<uint64_t>(
        std::llround(seconds * static_cast<double>(tickResolution) * bpm / 60.0));
}

uint32_t bendValue(double semitones) {
    const auto scaled = kBendCentre
        + (semitones / kBendSemitoneRange) * kBendCentre;
    return static_cast<uint32_t>(std::clamp(scaled, 0.0, 4294967295.0));
}

// Every message the writer emits is a 64-bit UMP, so the clip is assembled as
// whole messages and only flattened to words once the order is settled.
struct Message {
    uint64_t tick;
    int priority; // orders messages landing on the same tick
    uint64_t ump;
};

} // namespace

std::pair<std::vector<uapmd_ump_t>, std::vector<uint64_t>> makeNoteClip(
        const std::vector<TranscribedNote>& notes,
        uint32_t tick_resolution,
        double bpm) {
    std::vector<Message> messages;
    messages.reserve(notes.size() * 2);

    for (const auto& note : notes) {
        const auto onTick = secondsToTicks(note.start_seconds, tick_resolution, bpm);
        const auto offTick = secondsToTicks(note.end_seconds, tick_resolution, bpm);
        const auto velocity16 = static_cast<uint16_t>(
            std::lround(std::clamp(note.velocity, 0.0f, 1.0f) * 65535.0f));
        const auto attribute = umppi::UmpFactory::pitch7_9(note.detected_semitones);

        messages.push_back({onTick, 1, umppi::UmpFactory::midi2NoteOn(
            kGroup, kChannel, note.note_number,
            kPitch7_9AttributeType, velocity16, attribute)});

        // Bend points are relative to the note's start, and are emitted between
        // its Note On and Note Off. A point at or past the note's end is
        // dropped rather than clamped: it would belong to whatever plays next,
        // and one landing exactly on the end tick would be ordered after the
        // Note Off that silences the note it was meant to bend.
        for (const auto& point : note.bend) {
            const auto tick = secondsToTicks(
                note.start_seconds + point.seconds, tick_resolution, bpm);
            if (tick >= offTick)
                continue;
            messages.push_back({tick, 2, umppi::UmpFactory::midi2PerNotePitchBendDirect(
                kGroup, kChannel, note.note_number, bendValue(point.semitones))});
        }

        messages.push_back({offTick, 0, umppi::UmpFactory::midi2NoteOff(
            kGroup, kChannel, note.note_number,
            kPitch7_9AttributeType, 0, attribute)});
    }

    // Notes are produced in start order, but a long note's Note Off can land
    // after a later note's Note On, so the whole stream is ordered by tick. On
    // a shared tick a Note Off goes first, so that a repeated note ends before
    // it starts again, and bend points go last so they apply to the note just
    // started.
    std::ranges::stable_sort(messages, [](const Message& left, const Message& right) {
        if (left.tick != right.tick)
            return left.tick < right.tick;
        return left.priority < right.priority;
    });

    std::vector<uapmd_ump_t> events;
    std::vector<uint64_t> ticks;
    events.reserve(messages.size() * 2);
    ticks.reserve(messages.size() * 2);
    for (const auto& message : messages) {
        events.push_back(static_cast<uint32_t>(message.ump >> 32));
        ticks.push_back(message.tick);
        events.push_back(static_cast<uint32_t>(message.ump & 0xFFFFFFFFu));
        ticks.push_back(message.tick);
    }
    return {std::move(events), std::move(ticks)};
}

} // namespace uapmd_pitch
