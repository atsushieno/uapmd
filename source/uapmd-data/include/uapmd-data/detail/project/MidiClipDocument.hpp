#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>
#include "ProjectDocumentEvents.hpp"

namespace uapmd {

// Model-thread authored content. Existing field names are retained for editor compatibility.
struct MidiAutomationEvent {
    enum class Type : uint8_t {
        PitchBend,
        PerNotePitchBend,
        ChannelPressure,
        PolyPressure,
        ControlChange,
        RPN,
        NRPN,
        PerNoteParameter,
    };
    ProjectObjectId reference_id;
    bool registered_per_note{false};
    double timeSeconds{0.0};
    double normalizedValue{0.0};
    Type type{Type::ControlChange};
    uint8_t channel{0};
    uint8_t noteNumber{0};
    uint8_t umpGroup{0};
    uint16_t paramIndex{0};
    size_t rawEventIdx{SIZE_MAX};
};

struct MidiNote {
    double startSeconds{0.0};
    double durationSeconds{0.0};
    uint8_t note{0};
    float velocity{0.0f};
    uint8_t channel{0};
    bool deleted{false};
    size_t noteOnWordIdx{SIZE_MAX};
    size_t noteOffWordIdx{SIZE_MAX};
};

struct MidiRawData {
    std::vector<uapmd_ump_t> umpEvents;
    std::vector<uint64_t> tickTimestamps;
    uint32_t tickResolution{480};
    double clipTempo{120.0};
};

struct MidiEditNote : MidiNote {
    MidiEditNote() = default;
    explicit MidiEditNote(const MidiNote& base) : MidiNote(base) {}
    ProjectObjectId reference_id;
    uint64_t edit_id{0};
    uint8_t ump_group{0};
    uint16_t release_velocity{0};
    uint8_t attributeType{0};
    uint16_t attributeValue{0};
    std::vector<MidiAutomationEvent> automationEvents;
};


struct MidiClipContentSnapshot {
    MidiRawData raw;
    std::vector<MidiEditNote> notes;
    std::vector<MidiAutomationEvent> automation;
};

// Shared by all editors of a clip; independent of window lifetime and selection.
class MidiClipDocument {
    MidiClipContentSnapshot committed_;
    // committedOffsets maps a message's word offset in raw to its offset in the committed raw.
    MidiClipContentSnapshot withIdentitiesFrom(MidiRawData raw, double durationSeconds,
        const std::unordered_map<size_t, size_t>& committedOffsets) const;
public:
    std::shared_ptr<MidiRawData> rawMidiData;
    std::vector<MidiEditNote> editNotes;
    std::vector<MidiAutomationEvent> editClipEvents;
    uint64_t next_note_id{1};

    void load(MidiRawData raw, double durationSeconds);
    void restore(MidiClipContentSnapshot snapshot, bool mintIdentities = false);
    void ensureIdentities();
    std::optional<MidiClipContentSnapshot> withAppendedEvents(MidiRawData incoming) const;
    // Notes keep their identities while their note-on message lies outside the changed span; points likewise.
    MidiClipContentSnapshot withReplacedEvents(MidiRawData incoming, double durationSeconds) const;
    MidiClipContentSnapshot encodedSnapshot(const std::vector<size_t>& deletedRawIdxs = {});
    MidiClipContentSnapshot snapshot() const;
    const MidiClipContentSnapshot& committedSnapshot() const { return committed_; }
    void markCommitted();
    bool matchesSource(const MidiRawData& incoming) const;
    static MidiRawData normalize(MidiRawData raw);
    static void parseAutomationFromRaw(const MidiRawData& raw,
        std::vector<MidiEditNote>& notes, std::vector<MidiAutomationEvent>& events);
    static void seedNoteAttributesFromRaw(const MidiRawData& raw, std::vector<MidiEditNote>& notes);
};

}
