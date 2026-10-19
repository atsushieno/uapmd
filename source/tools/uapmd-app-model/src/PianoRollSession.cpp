#include "uapmd-app-model/uapmd-app-model.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <umppi/umppi.hpp>

namespace uapmd_app {

void PianoRollSession::publishSelection() {
    if (!project)
        return;
    content->ensureIdentities();
    uapmd::ProjectSelection selection;
    if (selectedNoteIdx >= 0 && selectedNoteIdx < static_cast<int>(editNotes.size()) &&
        !editNotes[selectedNoteIdx].deleted && selected_notes.contains(editNotes[selectedNoteIdx].edit_id))
        selection.objects.push_back(editNotes[selectedNoteIdx].reference_id);
    for (const auto& note : editNotes)
        if (!note.deleted && selected_notes.contains(note.edit_id) &&
            std::find(selection.objects.begin(), selection.objects.end(), note.reference_id) == selection.objects.end())
            selection.objects.push_back(note.reference_id);
    project->selections().replace(selection_context, std::move(selection));
    project->selections().setActiveContext(selection_context);
}

void PianoRollSession::syncSelection() {
    if (!project)
        return;
    const auto selection = project->selections().snapshot(selection_context);
    selected_notes.clear();
    selectedNoteIdx = -1;
    if (!selection)
        return;
    for (const auto& id : selection->objects)
        for (int i = 0; i < static_cast<int>(editNotes.size()); ++i)
            if (!editNotes[i].deleted && editNotes[i].reference_id == id) {
                selected_notes.insert(editNotes[i].edit_id);
                if (selectedNoteIdx < 0)
                    selectedNoteIdx = i;
            }
    if (selectedNoteIdx < 0)
        for (const auto& id : selection->objects)
            for (int i = 0; i < static_cast<int>(editNotes.size()); ++i)
                for (const auto& point : editNotes[i].automationEvents)
                    if (point.reference_id == id && !editNotes[i].deleted) {
                        selectedNoteIdx = i;
                        return;
                    }
}

bool PianoRollSession::isObjectSelected(const uapmd::ProjectObjectId& id) const {
    if (!project)
        return false;
    auto selection = project->selections().snapshot(selection_context);
    return selection && std::find(selection->objects.begin(), selection->objects.end(), id) != selection->objects.end();
}

bool PianoRollSession::isAutomationInSelectedRegion(const PianoRollAutomationEvent& event,
        std::optional<uapmd::ProjectObjectId> noteId) const {
    if (!project || !rawMidiData)
        return false;
    auto selection = project->selections().snapshot(selection_context);
    if (!selection)
        return false;
    using T = PianoRollAutomationEvent::Type;
    uint8_t status = 0xB0;
    switch (event.type) {
        case T::PitchBend: status = 0xE0; break;
        case T::PerNotePitchBend: status = 0x60; break;
        case T::ChannelPressure: status = 0xD0; break;
        case T::PolyPressure: status = 0xA0; break;
        case T::ControlChange: status = 0xB0; break;
        case T::RPN: status = 0x20; break;
        case T::NRPN: status = 0x30; break;
        case T::PerNoteParameter: status = event.registered_per_note ? 0x00 : 0x10; break;
    }
    const double tick = event.timeSeconds * std::max(1.0, rawMidiData->clipTempo) * rawMidiData->tickResolution / 60.0;
    for (const auto& value : selection->regions)
        if (auto* region = std::get_if<uapmd::MidiAutomationSelectionRegion>(&value)) {
            const auto& lane = region->lane;
            const bool inTime = region->start_tick == region->end_tick ?
                std::abs(tick - region->start_tick) < 1e-6 : tick >= region->start_tick && tick < region->end_tick;
            if (lane.clip_id == clip_reference_id && lane.note_id == noteId && lane.group == event.umpGroup &&
                lane.channel == event.channel && lane.status == status && lane.parameter == event.paramIndex && inTime &&
                (!region->values || (event.normalizedValue >= region->values->first && event.normalizedValue <= region->values->second)))
                return true;
        }
    return false;
}

void PianoRollSession::selectObject(const uapmd::ProjectObjectId& id, bool additive, bool toggle) {
    if (!project)
        return;
    auto& service = project->selections();
    auto selection = additive ? service.snapshot(selection_context).value_or(uapmd::ProjectSelection{}) : uapmd::ProjectSelection{};
    auto it = std::find(selection.objects.begin(), selection.objects.end(), id);
    if (it == selection.objects.end())
        selection.objects.insert(selection.objects.begin(), id);
    else if (toggle)
        selection.objects.erase(it);
    service.replace(selection_context, std::move(selection));
    service.setActiveContext(selection_context);
    syncSelection();
}

void PianoRollSession::parseAutomationFromRaw(const PianoRollRawMidiData& raw,
        std::vector<PianoRollEditNote>& notes, std::vector<PianoRollAutomationEvent>& events) {
    uapmd::MidiClipDocument::parseAutomationFromRaw(raw, notes, events);
}

void PianoRollSession::seedNoteAttributesFromRaw(const PianoRollRawMidiData& raw,
        std::vector<PianoRollEditNote>& notes) {
    uapmd::MidiClipDocument::seedNoteAttributesFromRaw(raw, notes);
}

void PianoRollSession::selectNote(int index, bool additive, bool toggle) {
    if (!additive)
        selected_notes.clear();
    if (index < 0 || index >= static_cast<int>(editNotes.size()) || editNotes[index].deleted) {
        selectedNoteIdx = -1;
        publishSelection();
        return;
    }
    const auto id = editNotes[index].edit_id;
    if (toggle && selected_notes.contains(id)) {
        selected_notes.erase(id);
        selectedNoteIdx = -1;
    } else {
        selected_notes.insert(id);
        selectedNoteIdx = index;
    }
    publishSelection();
}

void PianoRollSession::performAction(Action action, double pasteSeconds) {
    if (action == Action::SelectAll) {
        selected_notes.clear();
        selectedNoteIdx = -1;
        for (int i = 0; i < static_cast<int>(editNotes.size()); ++i)
            if (!editNotes[i].deleted)
                selectNote(i, true);
        return;
    }
    if (action == Action::Copy || action == Action::Cut) {
        std::vector<PianoRollEditNote> copied;
        double firstTime = std::numeric_limits<double>::max();
        for (const auto& note : editNotes)
            if (!note.deleted && selected_notes.contains(note.edit_id)) {
                copied.push_back(note);
                firstTime = std::min(firstTime, note.startSeconds);
            }
        if (copied.empty())
            return;
        for (auto& note : copied) {
            note.startSeconds -= firstTime;
            note.edit_id = 0;
            note.reference_id.clear();
            note.noteOnWordIdx = SIZE_MAX;
            note.noteOffWordIdx = SIZE_MAX;
            for (auto& event : note.automationEvents) {
                event.timeSeconds -= firstTime;
                event.rawEventIdx = SIZE_MAX;
                event.reference_id.clear();
            }
        }
        clipboard = std::move(copied);
    }
    if (action == Action::Cut || action == Action::Delete) {
        for (auto& note : editNotes)
            if (!note.deleted && selected_notes.contains(note.edit_id)) {
                note.deleted = true;
                dirtyAfterEdit = true;
            }
        selected_notes.clear();
        selectedNoteIdx = -1;
    } else if (action == Action::Paste && !clipboard.empty()) {
        selected_notes.clear();
        for (auto note : clipboard) {
            note.edit_id = next_note_id++;
            note.startSeconds += pasteSeconds;
            for (auto& event : note.automationEvents) {
                event.timeSeconds += pasteSeconds;
                event.reference_id.clear();
            }
            editNotes.push_back(std::move(note));
            selectNote(static_cast<int>(editNotes.size()) - 1, true);
        }
        dirtyAfterEdit = true;
    }
    publishSelection();
}

void PianoRollSession::moveNotes(
        const std::vector<std::pair<int, PianoRollEditNote>>& originalNotes,
        double timeDelta, int pitchDelta) {
    if (originalNotes.empty())
        return;
    double earliest = std::numeric_limits<double>::max();
    int lowest = 127, highest = 0;
    for (const auto& [index, original] : originalNotes) {
        earliest = std::min(earliest, original.startSeconds);
        for (const auto& event : original.automationEvents)
            earliest = std::min(earliest, event.timeSeconds);
        lowest = std::min(lowest, static_cast<int>(original.note));
        highest = std::max(highest, static_cast<int>(original.note));
    }
    timeDelta = std::max(timeDelta, -earliest);
    pitchDelta = std::clamp(pitchDelta, -lowest, 127 - highest);
    for (const auto& [index, original] : originalNotes) {
        auto& note = editNotes[index];
        note.startSeconds = original.startSeconds + timeDelta;
        note.note = static_cast<uint8_t>(original.note + pitchDelta);
        for (size_t i = 0; i < note.automationEvents.size(); ++i) {
            note.automationEvents[i].timeSeconds = original.automationEvents[i].timeSeconds + timeDelta;
            note.automationEvents[i].noteNumber = note.note;
        }
    }
}

void PianoRollSession::restoreNotes(
        const std::vector<std::pair<int, PianoRollEditNote>>& originalNotes) {
    for (const auto& [index, original] : originalNotes)
        if (index >= 0 && index < static_cast<int>(editNotes.size()))
            editNotes[index] = original;
}

void PianoRollSession::resizeNote(int index, double startSeconds,
                                 double durationSeconds, uint8_t noteNumber) {
    if (index < 0 || index >= static_cast<int>(editNotes.size()))
        return;
    auto& note = editNotes[index];
    note.startSeconds = std::max(0.0, startSeconds);
    note.durationSeconds = std::max(0.01, durationSeconds);
    note.note = noteNumber;
}

void PianoRollSession::finishNoteDrag(int index, double originalStart,
                                     double originalEnd, uint8_t originalNote) {
    if (index < 0 || index >= static_cast<int>(editNotes.size()))
        return;
    const auto& note = editNotes[index];
    if (note.startSeconds != originalStart ||
            note.startSeconds + note.durationSeconds != originalEnd ||
            note.note != originalNote)
        dirtyAfterEdit = true;
}

void PianoRollSession::createNote(double startSeconds, double durationSeconds,
                                 uint8_t noteNumber, float velocity) {
    PianoRollEditNote note;
    note.startSeconds = std::max(0.0, startSeconds);
    note.durationSeconds = std::max(0.01, durationSeconds);
    note.note = std::min<uint8_t>(noteNumber, 127);
    note.velocity = std::clamp(velocity, 0.0f, 1.0f);
    note.edit_id = next_note_id++;
    note.channel = editNotes.empty() ? 0 : editNotes.front().channel;
    note.ump_group = editNotes.empty() ? 0 : editNotes.front().ump_group;
    editNotes.push_back(std::move(note));
    selectNote(static_cast<int>(editNotes.size()) - 1);
    dirtyAfterEdit = true;
}

void PianoRollSession::deleteNote(int index) {
    if (index < 0 || index >= static_cast<int>(editNotes.size()) || editNotes[index].deleted)
        return;
    editNotes[index].deleted = true;
    selected_notes.erase(editNotes[index].edit_id);
    if (selectedNoteIdx == index)
        selectedNoteIdx = -1;
    dirtyAfterEdit = true;
}

void PianoRollSession::loadNotes(const std::vector<PianoRollMidiNote>& notes,
                                std::shared_ptr<PianoRollRawMidiData> raw,
                                double durationSeconds) {
    // The document owns identities; source reloads have already reconciled it.
    (void) raw;
    clipDurationSeconds = durationSeconds;
    (void) notes;
    selected_notes.clear();
    selectedNoteIdx = -1;
    deletedRawIdxs.clear();
    retry_available = false;
    edit_error.clear();
    dirtyAfterEdit = false;
    minNote = editNotes.empty() ? 48 : 127;
    maxNote = editNotes.empty() ? 72 : 0;
    for (const auto& note : editNotes) {
        minNote = std::min(minNote, note.note);
        maxNote = std::max(maxNote, note.note);
    }
}

bool PianoRollSession::matchesSource(const PianoRollRawMidiData& incoming) const {
    return content->matchesSource(incoming);
}


bool PianoRollSession::commit(AppModel& app) {
    dirtyAfterEdit = false;
    if (!rawMidiData || !project) {
        edit_error = "MIDI clip data is unavailable.";
        return false;
    }
    const auto snapshot = content->encodedSnapshot(deletedRawIdxs);
    if (!project->commands().setMidiClipContent(clip_reference_id, snapshot)) {
        edit_error = "Could not save note edits.";
        retry_available = true;
        return false;
    }
    edit_error.clear();
    retry_available = false;
    deletedRawIdxs.clear();
    syncSelection();
    minNote = editNotes.empty() ? 48 : 127;
    maxNote = editNotes.empty() ? 72 : 0;
    for (const auto& note : editNotes) {
        minNote = std::min(minNote, note.note);
        maxNote = std::max(maxNote, note.note);
    }
    if (auto clip = project->view().getClip(clip_reference_id))
        clipDurationSeconds = std::max(0.01, (clip->sourceOffsetSamples + clip->durationSamples) / std::max(1.0, clip->sampleRate));
    for (const auto& note : editNotes)
        clipDurationSeconds = std::max(clipDurationSeconds, note.startSeconds + note.durationSeconds);
    (void) app;
    return true;
}


PianoRollClipSnapshot AppModel::pianoRollClipSnapshot(
    int32_t trackIndex,
    const uapmd::ClipData& clipData,
    double fallbackDurationSeconds
) {
    PianoRollClipSnapshot snapshot;

    double durationSeconds = static_cast<double>(clipData.sourceOffsetSamples + clipData.durationSamples) /
        std::max(1.0, static_cast<double>(sampleRate()));
    if (durationSeconds <= 0.0) {
        durationSeconds = fallbackDurationSeconds;
    }
    snapshot.durationSeconds = std::max(0.01, durationSeconds);

    (void) trackIndex;
    auto content = sequencer_.engine()->timeline().document().midiContent(clipData.referenceId);
    if (!content) {
        snapshot.error = "Missing MIDI content";
        return snapshot;
    }
    const auto& committed = content->committedSnapshot();
    snapshot.rawMidiData = std::make_shared<PianoRollRawMidiData>(committed.raw);
    uint8_t minNote = 127, maxNote = 0;
    for (const auto& note : committed.notes)
        if (!note.deleted) {
            snapshot.notes.push_back(static_cast<const PianoRollMidiNote&>(note));
            snapshot.durationSeconds = std::max(snapshot.durationSeconds, note.startSeconds + note.durationSeconds);
            minNote = std::min(minNote, note.note);
            maxNote = std::max(maxNote, note.note);
        }
    if (!snapshot.notes.empty()) {
        snapshot.minNote = minNote;
        snapshot.maxNote = maxNote;
    }
    snapshot.ready = true;
    return snapshot;
}


} // namespace uapmd_app
