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

using PianoRollAutomationEvent = uapmd::MidiAutomationEvent;
using PianoRollMidiNote = uapmd::MidiNote;
using PianoRollRawMidiData = uapmd::MidiRawData;
using PianoRollEditNote = uapmd::MidiEditNote;

struct PianoRollSession {
    enum class Action { None, Copy, Cut, Paste, Delete, SelectAll };
    int32_t trackIndex{-1};
    int32_t clipId{-1};
    std::shared_ptr<uapmd::MidiClipDocument> content;
    std::shared_ptr<PianoRollRawMidiData>& rawMidiData;
    std::vector<PianoRollEditNote>& editNotes;
    std::vector<PianoRollAutomationEvent>& editClipEvents;
    uint64_t& next_note_id;
    uapmd::ProjectDocument* project{};
    uapmd::ProjectObjectId clip_reference_id;
    uapmd::SelectionContextId selection_context{0};

    explicit PianoRollSession(std::shared_ptr<uapmd::MidiClipDocument> document)
        : content(std::move(document)), rawMidiData(content->rawMidiData),
          editNotes(content->editNotes), editClipEvents(content->editClipEvents),
          next_note_id(content->next_note_id) {}
    void publishSelection();
    void syncSelection();
    bool isObjectSelected(const uapmd::ProjectObjectId& id) const;
    bool isAutomationInSelectedRegion(const PianoRollAutomationEvent& event,
        std::optional<uapmd::ProjectObjectId> noteId = {}) const;
    void selectObject(const uapmd::ProjectObjectId& id, bool additive, bool toggle);
    std::unordered_set<uint64_t> selected_notes;
    int selectedNoteIdx{-1};
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
