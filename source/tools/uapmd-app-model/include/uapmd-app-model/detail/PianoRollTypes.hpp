#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <uapmd-data/uapmd-data.hpp>

namespace uapmd_app {

struct PianoRollAutomationEvent {
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
    double timeSeconds{0.0};
    double normalizedValue{0.0};
    Type type{Type::ControlChange};
    uint8_t channel{0};
    uint8_t noteNumber{0};
    uint8_t umpGroup{0};
    uint16_t paramIndex{0};
    size_t rawEventIdx{SIZE_MAX};
};

struct PianoRollMidiNote {
    double startSeconds{0.0};
    double durationSeconds{0.0};
    uint8_t note{0};
    float velocity{0.0f};
    uint8_t channel{0};
    bool deleted{false};
    size_t noteOnWordIdx{SIZE_MAX};
    size_t noteOffWordIdx{SIZE_MAX};
};

struct PianoRollRawMidiData {
    std::vector<uapmd_ump_t> umpEvents;
    std::vector<uint64_t> tickTimestamps;
    uint32_t tickResolution{480};
    double clipTempo{120.0};
};

struct PianoRollEditNote : PianoRollMidiNote {
    PianoRollEditNote() = default;
    explicit PianoRollEditNote(const PianoRollMidiNote& base) : PianoRollMidiNote(base) {}
    uint64_t edit_id{0};
    uint8_t ump_group{0};
    uint16_t release_velocity{0};
    uint8_t attributeType{0};
    uint16_t attributeValue{0};
    std::vector<PianoRollAutomationEvent> automationEvents;
};

struct PianoRollSession {
    enum class Action { None, Copy, Cut, Paste, Delete, SelectAll };
    int32_t trackIndex{-1};
    int32_t clipId{-1};
    std::shared_ptr<PianoRollRawMidiData> rawMidiData;
    std::vector<PianoRollEditNote> editNotes;
    std::vector<PianoRollAutomationEvent> editClipEvents;
    std::unordered_set<uint64_t> selected_notes;
    int selectedNoteIdx{-1};
    uint64_t next_note_id{1};
    std::vector<PianoRollEditNote> clipboard;
    std::vector<size_t> deletedRawIdxs;
    bool dirtyAfterEdit{false};
    bool retry_available{false};
    std::string edit_error;
    double clipDurationSeconds{0.01};
    uint8_t minNote{48};
    uint8_t maxNote{72};

    void selectNote(int index, bool additive = false, bool toggle = false);
    void performAction(Action action, double pasteSeconds);
    void moveNotes(const std::vector<std::pair<int, PianoRollEditNote>>& originalNotes,
                   double timeDelta, int pitchDelta);
    void restoreNotes(const std::vector<std::pair<int, PianoRollEditNote>>& originalNotes);
    void resizeNote(int index, double startSeconds, double durationSeconds, uint8_t noteNumber);
    void finishNoteDrag(int index, double originalStart, double originalEnd, uint8_t originalNote);
    void createNote(double startSeconds, double durationSeconds, uint8_t noteNumber, float velocity);
    void deleteNote(int index);
    void loadNotes(const std::vector<PianoRollMidiNote>& notes,
                   std::shared_ptr<PianoRollRawMidiData> raw,
                   double durationSeconds);
    bool matchesSource(const PianoRollRawMidiData& incoming) const;
    bool commit(class AppModel& app);
    static void parseAutomationFromRaw(const PianoRollRawMidiData& raw,
                                       std::vector<PianoRollEditNote>& notes,
                                       std::vector<PianoRollAutomationEvent>& clipEvents);
    static void seedNoteAttributesFromRaw(const PianoRollRawMidiData& raw,
                                          std::vector<PianoRollEditNote>& notes);
};

struct PianoRollClipSnapshot {
    bool ready{false};
    std::string error;
    double durationSeconds{0.01};
    std::shared_ptr<PianoRollRawMidiData> rawMidiData;
    std::vector<PianoRollMidiNote> notes;
    uint8_t minNote{48};
    uint8_t maxNote{72};
};

} // namespace uapmd_app
