#include "uapmd-app-model/uapmd-app-model.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <umppi/umppi.hpp>

namespace uapmd_app {

void PianoRollSession::selectNote(int index, bool additive, bool toggle) {
    if (!additive)
        selected_notes.clear();
    if (index < 0 || index >= static_cast<int>(editNotes.size()) || editNotes[index].deleted) {
        selectedNoteIdx = -1;
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
            note.noteOnWordIdx = SIZE_MAX;
            note.noteOffWordIdx = SIZE_MAX;
            for (auto& event : note.automationEvents) {
                event.timeSeconds -= firstTime;
                event.rawEventIdx = SIZE_MAX;
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
            for (auto& event : note.automationEvents)
                event.timeSeconds += pasteSeconds;
            editNotes.push_back(std::move(note));
            selectNote(static_cast<int>(editNotes.size()) - 1, true);
        }
        dirtyAfterEdit = true;
    }
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

void PianoRollSession::parseAutomationFromRaw(
        const PianoRollRawMidiData& raw,
        std::vector<PianoRollEditNote>&                      editNotes,
        std::vector<PianoRollAutomationEvent>&  clipEvents) {
    clipEvents.clear();
    for (auto& n : editNotes)
        n.automationEvents.clear();

    const auto& events = raw.umpEvents;
    const auto& ticks  = raw.tickTimestamps;
    if (events.empty()) return;

    const uint32_t tickRes  = raw.tickResolution > 0 ? raw.tickResolution : 480;
    const double   bpm      = raw.clipTempo > 0.0 ? raw.clipTempo : 120.0;
    const double   secPerTick = 60.0 / (static_cast<double>(tickRes) * bpm);

    // Map from the first-word raw index of a NoteOn event back to its editNote slot.
    std::unordered_map<size_t, size_t> wordIdxToNoteIdx;
    wordIdxToNoteIdx.reserve(editNotes.size());
    for (size_t ni = 0; ni < editNotes.size(); ++ni)
        if (editNotes[ni].noteOnWordIdx != SIZE_MAX)
            wordIdxToNoteIdx[editNotes[ni].noteOnWordIdx] = ni;

    // (group<<12|ch<<7|note) → currently-active editNote index.
    std::unordered_map<uint32_t, size_t> activeNoteIndices;
    activeNoteIndices.reserve(64);

    auto addPerNoteEvt = [&](uint32_t noteKey, PianoRollAutomationEvent evt) {
        auto it = activeNoteIndices.find(noteKey);
        if (it != activeNoteIndices.end())
            editNotes[it->second].automationEvents.push_back(std::move(evt));
    };

    const size_t eventCount = std::min(events.size(), ticks.size());
    size_t i = 0;
    while (i < eventCount) {
        umppi::Ump ump1(events[i]);
        const int  wordCount = ump1.getSizeInInts();
        const size_t safeCount = std::min(static_cast<size_t>(wordCount), eventCount - i);
        umppi::Ump ump = (safeCount >= 2) ? umppi::Ump(events[i], events[i + 1]) : ump1;

        const double t = static_cast<double>(ticks[i]) * secPerTick;
        const auto msgType = ump.getMessageType();

        if (msgType == umppi::MessageType::MIDI1) {
            const uint8_t  status  = ump.getStatusCode();
            const uint8_t  channel = ump.getChannelInGroup();
            const uint8_t  group   = ump.getGroup();
            const uint32_t baseKey = (static_cast<uint32_t>(group) << 12) |
                                     (static_cast<uint32_t>(channel) << 7);

            if (status == umppi::MidiChannelStatus::NOTE_ON) {
                const uint8_t  note  = ump.getMidi1Note();
                const uint8_t  vel   = ump.getMidi1Velocity();
                const uint32_t nk    = baseKey | note;
                if (vel > 0) {
                    auto it = wordIdxToNoteIdx.find(i);
                    if (it != wordIdxToNoteIdx.end()) activeNoteIndices[nk] = it->second;
                } else {
                    activeNoteIndices.erase(nk);
                }
            } else if (status == umppi::MidiChannelStatus::NOTE_OFF) {
                activeNoteIndices.erase(baseKey | ump.getMidi1Note());
            } else if (status == umppi::MidiChannelStatus::PAF) {
                const uint8_t note = ump.getMidi1Msb();
                PianoRollAutomationEvent evt{};
                evt.timeSeconds     = t;
                evt.normalizedValue = ump.getMidi1Lsb() / 127.0;
                evt.type            = PianoRollAutomationEvent::Type::PolyPressure;
                evt.channel         = channel;
                evt.noteNumber      = note;
                evt.rawEventIdx     = i;
                addPerNoteEvt(baseKey | note, evt);
            } else if (status == umppi::MidiChannelStatus::CC) {
                PianoRollAutomationEvent evt{};
                evt.timeSeconds     = t;
                evt.normalizedValue = ump.getMidi1CCData() / 127.0;
                evt.type            = PianoRollAutomationEvent::Type::ControlChange;
                evt.channel         = channel;
                evt.paramIndex      = ump.getMidi1CCIndex();
                evt.rawEventIdx     = i;
                clipEvents.push_back(evt);
            } else if (status == umppi::MidiChannelStatus::CAF) {
                PianoRollAutomationEvent evt{};
                evt.timeSeconds     = t;
                evt.normalizedValue = ump.getMidi1Msb() / 127.0;
                evt.type            = PianoRollAutomationEvent::Type::ChannelPressure;
                evt.channel         = channel;
                evt.rawEventIdx     = i;
                clipEvents.push_back(evt);
            } else if (status == umppi::MidiChannelStatus::PITCH_BEND) {
                PianoRollAutomationEvent evt{};
                evt.timeSeconds     = t;
                evt.normalizedValue = ump.getMidi1PitchBendData() / 16383.0;
                evt.type            = PianoRollAutomationEvent::Type::PitchBend;
                evt.channel         = channel;
                evt.rawEventIdx     = i;
                clipEvents.push_back(evt);
            }
        } else if (msgType == umppi::MessageType::MIDI2) {
            const uint8_t  status  = ump.getStatusCode();
            const uint8_t  channel = ump.getChannelInGroup();
            const uint8_t  group   = ump.getGroup();
            const uint32_t baseKey = (static_cast<uint32_t>(group) << 12) |
                                     (static_cast<uint32_t>(channel) << 7);

            if (status == umppi::MidiChannelStatus::NOTE_ON) {
                const uint8_t  note = ump.getMidi2Note();
                const uint16_t vel  = ump.getMidi2Velocity16();
                const uint32_t nk   = baseKey | note;
                if (vel > 0) {
                    auto it = wordIdxToNoteIdx.find(i);
                    if (it != wordIdxToNoteIdx.end()) activeNoteIndices[nk] = it->second;
                } else {
                    activeNoteIndices.erase(nk);
                }
            } else if (status == umppi::MidiChannelStatus::NOTE_OFF) {
                activeNoteIndices.erase(baseKey | ump.getMidi2Note());
            } else if (status == umppi::MidiChannelStatus::PAF) {
                const uint8_t note = ump.getMidi2Note();
                PianoRollAutomationEvent evt{};
                evt.timeSeconds     = t;
                evt.normalizedValue = ump.getMidi2PafData() / static_cast<double>(0xFFFFFFFFu);
                evt.type            = PianoRollAutomationEvent::Type::PolyPressure;
                evt.channel         = channel;
                evt.noteNumber      = note;
                evt.rawEventIdx     = i;
                addPerNoteEvt(baseKey | note, evt);
            } else if (status == umppi::MidiChannelStatus::PER_NOTE_PITCH_BEND) {
                const uint8_t note = ump.getMidi2Note();
                PianoRollAutomationEvent evt{};
                evt.timeSeconds     = t;
                evt.normalizedValue = ump.getMidi2PitchBendData() / static_cast<double>(0xFFFFFFFFu);
                evt.type            = PianoRollAutomationEvent::Type::PerNotePitchBend;
                evt.channel         = channel;
                evt.noteNumber      = note;
                evt.rawEventIdx     = i;
                addPerNoteEvt(baseKey | note, evt);
            } else if (status == umppi::MidiChannelStatus::PER_NOTE_RCC ||
                       status == umppi::MidiChannelStatus::PER_NOTE_ACC) {
                const uint8_t note = ump.getMidi2Note();
                PianoRollAutomationEvent evt{};
                evt.timeSeconds     = t;
                evt.normalizedValue = ump.getMidi2CcData() / static_cast<double>(0xFFFFFFFFu);
                evt.type            = PianoRollAutomationEvent::Type::PerNoteParameter;
                evt.channel         = channel;
                evt.noteNumber      = note;
                evt.paramIndex      = static_cast<uint16_t>(events[i] & 0xFF);
                evt.rawEventIdx     = i;
                addPerNoteEvt(baseKey | note, evt);
            } else if (status == umppi::MidiChannelStatus::CC) {
                PianoRollAutomationEvent evt{};
                evt.timeSeconds     = t;
                evt.normalizedValue = ump.getMidi2CcData() / static_cast<double>(0xFFFFFFFFu);
                evt.type            = PianoRollAutomationEvent::Type::ControlChange;
                evt.channel         = channel;
                evt.paramIndex      = ump.getMidi2CcIndex();
                evt.rawEventIdx     = i;
                clipEvents.push_back(evt);
            } else if (status == umppi::MidiChannelStatus::RPN) {
                PianoRollAutomationEvent evt{};
                evt.timeSeconds     = t;
                evt.normalizedValue = ump.getMidi2RpnData() / static_cast<double>(0xFFFFFFFFu);
                evt.type            = PianoRollAutomationEvent::Type::RPN;
                evt.channel         = channel;
                evt.paramIndex      = static_cast<uint16_t>((ump.getMidi2RpnMsb() << 7) | ump.getMidi2RpnLsb());
                evt.rawEventIdx     = i;
                clipEvents.push_back(evt);
            } else if (status == umppi::MidiChannelStatus::NRPN) {
                PianoRollAutomationEvent evt{};
                evt.timeSeconds     = t;
                evt.normalizedValue = ump.getMidi2NrpnData() / static_cast<double>(0xFFFFFFFFu);
                evt.type            = PianoRollAutomationEvent::Type::NRPN;
                evt.channel         = channel;
                evt.umpGroup        = group;
                evt.paramIndex      = static_cast<uint16_t>((ump.getMidi2NrpnMsb() << 7) | ump.getMidi2NrpnLsb());
                evt.rawEventIdx     = i;
                clipEvents.push_back(evt);
            } else if (status == umppi::MidiChannelStatus::CAF) {
                PianoRollAutomationEvent evt{};
                evt.timeSeconds     = t;
                evt.normalizedValue = ump.getMidi2CafData() / static_cast<double>(0xFFFFFFFFu);
                evt.type            = PianoRollAutomationEvent::Type::ChannelPressure;
                evt.channel         = channel;
                evt.rawEventIdx     = i;
                clipEvents.push_back(evt);
            } else if (status == umppi::MidiChannelStatus::PITCH_BEND) {
                PianoRollAutomationEvent evt{};
                evt.timeSeconds     = t;
                evt.normalizedValue = ump.getMidi2PitchBendData() / static_cast<double>(0xFFFFFFFFu);
                evt.type            = PianoRollAutomationEvent::Type::PitchBend;
                evt.channel         = channel;
                evt.rawEventIdx     = i;
                clipEvents.push_back(evt);
            }
        }

        i += static_cast<size_t>(std::max(1, wordCount));
    }
}

void PianoRollSession::seedNoteAttributesFromRaw(const PianoRollRawMidiData& raw,
                                                std::vector<PianoRollEditNote>& editNotes) {
    for (auto& note : editNotes) {
        note.attributeType = 0;
        note.attributeValue = 0;
        if (note.noteOnWordIdx >= raw.umpEvents.size() || note.noteOnWordIdx + 1 >= raw.umpEvents.size())
            continue;
        note.ump_group = static_cast<uint8_t>((raw.umpEvents[note.noteOnWordIdx] >> 24) & 0xFu);
        note.channel = static_cast<uint8_t>((raw.umpEvents[note.noteOnWordIdx] >> 16) & 0xFu);
        if (note.noteOffWordIdx < raw.umpEvents.size() && note.noteOffWordIdx + 1 < raw.umpEvents.size())
            note.release_velocity = static_cast<uint16_t>(raw.umpEvents[note.noteOffWordIdx + 1] >> 16);
        note.attributeType = static_cast<uint8_t>(raw.umpEvents[note.noteOnWordIdx] & 0xFFu);
        note.attributeValue = static_cast<uint16_t>(raw.umpEvents[note.noteOnWordIdx + 1] & 0xFFFFu);
    }
}

void PianoRollSession::loadNotes(const std::vector<PianoRollMidiNote>& notes,
                                std::shared_ptr<PianoRollRawMidiData> raw,
                                double durationSeconds) {
    rawMidiData = std::move(raw);
    clipDurationSeconds = durationSeconds;
    editNotes.clear();
    editClipEvents.clear();
    editNotes.reserve(notes.size());
    for (const auto& note : notes)
        editNotes.emplace_back(note);
    if (rawMidiData) {
        seedNoteAttributesFromRaw(*rawMidiData, editNotes);
        parseAutomationFromRaw(*rawMidiData, editNotes, editClipEvents);
    }
    for (auto& note : editNotes)
        note.edit_id = next_note_id++;
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
    return rawMidiData && rawMidiData->umpEvents == incoming.umpEvents &&
        rawMidiData->tickTimestamps == incoming.tickTimestamps &&
        rawMidiData->tickResolution == incoming.tickResolution &&
        rawMidiData->clipTempo == incoming.clipTempo;
}


static uint64_t secondsToTicks(double seconds, uint32_t tickRes, double bpm) noexcept {
    if (bpm <= 0.0 || tickRes == 0 || seconds < 0.0) return 0;
    return static_cast<uint64_t>(std::round(seconds * static_cast<double>(tickRes) * bpm / 60.0));
}

static void sortRawMidiEvents(std::vector<uapmd_ump_t>& events,
                                         std::vector<uint64_t>&    ticks, std::vector<size_t>& wordOrder) {
    // Group consecutive words that form a single UMP message, sort groups by
    // tick, then flatten back to word-per-entry arrays.
    struct Group {
        uint64_t tick{0};
        size_t original_index{0};
        std::vector<uapmd_ump_t> words;
    };

    std::vector<Group> groups;
    groups.reserve(events.size());

    size_t i = 0;
    while (i < events.size()) {
        umppi::Ump ump(events[i]);
        int wordCount = std::max(1, ump.getSizeInInts());
        Group g;
        g.original_index = i;
        g.tick = (i < ticks.size()) ? ticks[i] : 0;
        size_t end = std::min(i + static_cast<size_t>(wordCount), events.size());
        for (size_t j = i; j < end; ++j)
            g.words.push_back(events[j]);
        groups.push_back(std::move(g));
        i += static_cast<size_t>(wordCount);
    }

    std::stable_sort(groups.begin(), groups.end(), [](const Group& a, const Group& b) {
        return a.tick < b.tick;
    });

    wordOrder.resize(events.size());
    events.clear();
    ticks.clear();
    for (const auto& g : groups) {
        size_t source = g.original_index;
        for (auto w : g.words) {
            wordOrder[source++] = events.size();
            events.push_back(w);
            ticks.push_back(g.tick);
        }
    }
}


bool PianoRollSession::commit(AppModel& app) {
    dirtyAfterEdit = false;
    if (!rawMidiData) {
        edit_error = "MIDI clip data is unavailable.";
        return false;
    }

    const PianoRollRawMidiData& orig = *rawMidiData;
    const uint32_t tickRes = orig.tickResolution;
    const double   bpm     = orig.clipTempo > 0.0 ? orig.clipTempo : 120.0;

    // Fallback group for clip-level automation.
    uint8_t defaultGroup = 0;
    if (!editNotes.empty()) {
        const size_t idx0 = editNotes[0].noteOnWordIdx;
        if (idx0 < orig.umpEvents.size()) {
            defaultGroup   = static_cast<uint8_t>((orig.umpEvents[idx0] >> 24) & 0xFu);
        }
    }

    // Mark all raw-event indices owned by tracked notes so we can exclude them
    // from the non-note pass.  We rebuild note events fresh from editNotes below.
    std::vector<bool> skipIdx(orig.umpEvents.size(), false);
    auto markSkip = [&](size_t start) {
        if (start >= orig.umpEvents.size()) return;
        umppi::Ump u(orig.umpEvents[start]);
        int sz = std::max(1, u.getSizeInInts());
        for (int w = 0; w < sz && start + w < orig.umpEvents.size(); ++w)
            skipIdx[start + w] = true;
    };
    // Skip note ON/OFF raw events and their associated automation events so we
    // can re-emit the edited copies below.  editNotes holds all original notes
    // (including deleted ones), so all original raw indices are covered.
    for (const auto& editNote : editNotes) {
        markSkip(editNote.noteOnWordIdx);
        markSkip(editNote.noteOffWordIdx);
        for (const auto& ae : editNote.automationEvents)
            markSkip(ae.rawEventIdx);
    }
    for (const auto& ae : editClipEvents)
        markSkip(ae.rawEventIdx);
    // Also skip raw events whose in-memory counterpart was deleted this frame.
    for (size_t idx : deletedRawIdxs)
        markSkip(idx);
    // Begin the new event list with all non-note events (CC, pitch-bend, …).
    std::vector<uapmd_ump_t> newEvents;
    std::vector<uint64_t>    newTicks;
    newEvents.reserve(orig.umpEvents.size());
    newTicks.reserve(orig.tickTimestamps.size());
    for (size_t i = 0; i < orig.umpEvents.size(); ++i) {
        if (!skipIdx[i]) {
            newEvents.push_back(orig.umpEvents[i]);
            newTicks.push_back(i < orig.tickTimestamps.size() ? orig.tickTimestamps[i] : 0);
        }
    }

    std::vector<std::pair<size_t, uint64_t>> emittedIds;
    std::unordered_map<uint64_t, size_t> emittedOffWords;
    std::unordered_map<uint64_t, std::vector<size_t>> emittedNoteAutomationWords;
    std::vector<size_t> emittedClipAutomationWords;
    const uint64_t primaryId = selectedNoteIdx >= 0 && selectedNoteIdx < static_cast<int>(editNotes.size())
        ? editNotes[selectedNoteIdx].edit_id : 0;
    // Emit NoteOn + NoteOff for each live (non-deleted) note.
    for (const auto& editNote : editNotes) {
        if (editNote.deleted) continue;

        const uint64_t onTick  = secondsToTicks(editNote.startSeconds, tickRes, bpm);
        const uint64_t offTick = secondsToTicks(
            editNote.startSeconds + editNote.durationSeconds, tickRes, bpm);

        const uint8_t grp = editNote.ump_group;
        const uint8_t ch = editNote.channel;
        emittedIds.emplace_back(newEvents.size(), editNote.edit_id);

        const uint16_t vel16  = static_cast<uint16_t>(
            std::round(std::clamp(editNote.velocity, 0.0f, 1.0f) * 65535.0f));
        const uint64_t onUmp  = umppi::UmpFactory::midi2NoteOn(
            grp, ch, editNote.note, editNote.attributeType, vel16, editNote.attributeValue);
        newEvents.push_back(static_cast<uint32_t>(onUmp >> 32));
        newTicks.push_back(onTick);
        newEvents.push_back(static_cast<uint32_t>(onUmp & 0xFFFFFFFFu));
        newTicks.push_back(onTick);

        emittedOffWords.emplace(editNote.edit_id, newEvents.size());
        const uint64_t offUmp = umppi::UmpFactory::midi2NoteOff(
            grp, ch, editNote.note, editNote.attributeType, editNote.release_velocity, editNote.attributeValue);
        newEvents.push_back(static_cast<uint32_t>(offUmp >> 32));
        newTicks.push_back(offTick);
        newEvents.push_back(static_cast<uint32_t>(offUmp & 0xFFFFFFFFu));
        newTicks.push_back(offTick);

        // Emit per-note automation events (edited or newly added).
        for (const auto& ae : editNote.automationEvents) {
            const size_t eventWord = newEvents.size();
            const uint64_t aeTick = secondsToTicks(ae.timeSeconds, tickRes, bpm);
            const double   v      = std::clamp(ae.normalizedValue, 0.0, 1.0);
            switch (ae.type) {
            case PianoRollAutomationEvent::Type::PitchBend: {
                const auto d14 = static_cast<uint16_t>(std::round(v * 16383.0));
                newEvents.push_back(umppi::UmpFactory::midi1PitchBendDirect(grp, ch, d14));
                newTicks.push_back(aeTick);
                break;
            }
            case PianoRollAutomationEvent::Type::PerNotePitchBend: {
                const auto d32 = static_cast<uint32_t>(std::round(v * 4294967295.0));
                const uint64_t u2 = umppi::UmpFactory::midi2PerNotePitchBendDirect(
                    grp, ch, editNote.note, d32);
                newEvents.push_back(static_cast<uint32_t>(u2 >> 32));
                newTicks.push_back(aeTick);
                newEvents.push_back(static_cast<uint32_t>(u2 & 0xFFFFFFFFu));
                newTicks.push_back(aeTick);
                break;
            }
            case PianoRollAutomationEvent::Type::ChannelPressure: {
                const auto d7 = static_cast<uint8_t>(std::round(v * 127.0));
                newEvents.push_back(umppi::UmpFactory::midi1CAf(grp, ch, d7));
                newTicks.push_back(aeTick);
                break;
            }
            case PianoRollAutomationEvent::Type::PolyPressure: {
                const auto d7 = static_cast<uint8_t>(std::round(v * 127.0));
                newEvents.push_back(umppi::UmpFactory::midi1PAf(grp, ch, editNote.note, d7));
                newTicks.push_back(aeTick);
                break;
            }
            case PianoRollAutomationEvent::Type::ControlChange: {
                const auto cc  = static_cast<uint8_t>(ae.paramIndex & 0x7Fu);
                const auto d32 = static_cast<uint32_t>(std::round(v * 4294967295.0));
                const uint64_t u2 = umppi::UmpFactory::midi2CC(grp, ch, cc, d32);
                newEvents.push_back(static_cast<uint32_t>(u2 >> 32));
                newTicks.push_back(aeTick);
                newEvents.push_back(static_cast<uint32_t>(u2 & 0xFFFFFFFFu));
                newTicks.push_back(aeTick);
                break;
            }
            case PianoRollAutomationEvent::Type::RPN: {
                const auto msb = static_cast<uint8_t>(ae.paramIndex >> 7);
                const auto lsb = static_cast<uint8_t>(ae.paramIndex & 0x7Fu);
                const auto d32 = static_cast<uint32_t>(std::round(v * 4294967295.0));
                const uint64_t u2 = umppi::UmpFactory::midi2RPN(grp, ch, msb, lsb, d32);
                newEvents.push_back(static_cast<uint32_t>(u2 >> 32));
                newTicks.push_back(aeTick);
                newEvents.push_back(static_cast<uint32_t>(u2 & 0xFFFFFFFFu));
                newTicks.push_back(aeTick);
                break;
            }
            case PianoRollAutomationEvent::Type::NRPN: {
                const auto msb = static_cast<uint8_t>(ae.paramIndex >> 7);
                const auto lsb = static_cast<uint8_t>(ae.paramIndex & 0x7Fu);
                const auto d32 = static_cast<uint32_t>(std::round(v * 4294967295.0));
                const uint64_t u2 = umppi::UmpFactory::midi2NRPN(ae.umpGroup, ch, msb, lsb, d32);
                newEvents.push_back(static_cast<uint32_t>(u2 >> 32));
                newTicks.push_back(aeTick);
                newEvents.push_back(static_cast<uint32_t>(u2 & 0xFFFFFFFFu));
                newTicks.push_back(aeTick);
                break;
            }
            case PianoRollAutomationEvent::Type::PerNoteParameter: {
                const auto d32 = static_cast<uint32_t>(std::round(v * 4294967295.0));
                const uint64_t u2 = umppi::UmpFactory::midi2PerNoteRCC(
                    grp, ch, editNote.note,
                    static_cast<uint8_t>(ae.paramIndex & 0xFFu), d32);
                newEvents.push_back(static_cast<uint32_t>(u2 >> 32));
                newTicks.push_back(aeTick);
                newEvents.push_back(static_cast<uint32_t>(u2 & 0xFFFFFFFFu));
                newTicks.push_back(aeTick);
                break;
            }
            }
            emittedNoteAutomationWords[editNote.edit_id].push_back(eventWord);
        }
    }

    // Re-emit edited clip-level automation events.
    // Per-note types can end up here when the user changes the type of a channel-level row:
    // recover the note number by finding whichever editNote contains the event's time.
    auto findNoteNum = [&](const PianoRollAutomationEvent& ae) -> uint8_t {
        if (ae.noteNumber != 0) return ae.noteNumber; // still valid (not yet round-tripped)
        for (const auto& en : editNotes) {
            if (!en.deleted && ae.timeSeconds >= en.startSeconds &&
                    ae.timeSeconds <= en.startSeconds + en.durationSeconds)
                return en.note;
        }
        return 0; // fallback — note association unknown
    };

    for (const auto& ae : editClipEvents) {
        const size_t eventWord = newEvents.size();
        const uint64_t aeTick = secondsToTicks(ae.timeSeconds, tickRes, bpm);
        const double   v      = std::clamp(ae.normalizedValue, 0.0, 1.0);
        // Use defaultGroup; channel comes from the stored AutomationEvent::channel.
        switch (ae.type) {
        case PianoRollAutomationEvent::Type::PitchBend: {
            const auto d14 = static_cast<uint16_t>(std::round(v * 16383.0));
            newEvents.push_back(umppi::UmpFactory::midi1PitchBendDirect(defaultGroup, ae.channel, d14));
            newTicks.push_back(aeTick);
            break;
        }
        case PianoRollAutomationEvent::Type::ChannelPressure: {
            const auto d7 = static_cast<uint8_t>(std::round(v * 127.0));
            newEvents.push_back(umppi::UmpFactory::midi1CAf(defaultGroup, ae.channel, d7));
            newTicks.push_back(aeTick);
            break;
        }
        case PianoRollAutomationEvent::Type::ControlChange: {
            const auto cc  = static_cast<uint8_t>(ae.paramIndex & 0x7Fu);
            const auto d32 = static_cast<uint32_t>(std::round(v * 4294967295.0));
            const uint64_t u2 = umppi::UmpFactory::midi2CC(defaultGroup, ae.channel, cc, d32);
            newEvents.push_back(static_cast<uint32_t>(u2 >> 32));
            newTicks.push_back(aeTick);
            newEvents.push_back(static_cast<uint32_t>(u2 & 0xFFFFFFFFu));
            newTicks.push_back(aeTick);
            break;
        }
        case PianoRollAutomationEvent::Type::RPN: {
            const auto msb = static_cast<uint8_t>(ae.paramIndex >> 7);
            const auto lsb = static_cast<uint8_t>(ae.paramIndex & 0x7Fu);
            const auto d32 = static_cast<uint32_t>(std::round(v * 4294967295.0));
            const uint64_t u2 = umppi::UmpFactory::midi2RPN(defaultGroup, ae.channel, msb, lsb, d32);
            newEvents.push_back(static_cast<uint32_t>(u2 >> 32));
            newTicks.push_back(aeTick);
            newEvents.push_back(static_cast<uint32_t>(u2 & 0xFFFFFFFFu));
            newTicks.push_back(aeTick);
            break;
        }
        case PianoRollAutomationEvent::Type::NRPN: {
            const auto msb = static_cast<uint8_t>(ae.paramIndex >> 7);
            const auto lsb = static_cast<uint8_t>(ae.paramIndex & 0x7Fu);
            const auto d32 = static_cast<uint32_t>(std::round(v * 4294967295.0));
            const uint64_t u2 = umppi::UmpFactory::midi2NRPN(ae.umpGroup, ae.channel, msb, lsb, d32);
            newEvents.push_back(static_cast<uint32_t>(u2 >> 32));
            newTicks.push_back(aeTick);
            newEvents.push_back(static_cast<uint32_t>(u2 & 0xFFFFFFFFu));
            newTicks.push_back(aeTick);
            break;
        }
        // Per-note types that migrated into editClipEvents after a channel-level round-trip.
        // Emit the proper per-note MIDI message so the parser re-associates them with
        // the parent note (via activeNoteIndices) on the next reload.
        case PianoRollAutomationEvent::Type::PolyPressure: {
            const uint8_t noteNum = findNoteNum(ae);
            const auto d7 = static_cast<uint8_t>(std::round(v * 127.0));
            newEvents.push_back(umppi::UmpFactory::midi1PAf(defaultGroup, ae.channel, noteNum, d7));
            newTicks.push_back(aeTick);
            break;
        }
        case PianoRollAutomationEvent::Type::PerNotePitchBend: {
            const uint8_t noteNum = findNoteNum(ae);
            const auto d32 = static_cast<uint32_t>(std::round(v * 4294967295.0));
            const uint64_t u2 = umppi::UmpFactory::midi2PerNotePitchBendDirect(
                defaultGroup, ae.channel, noteNum, d32);
            newEvents.push_back(static_cast<uint32_t>(u2 >> 32));
            newTicks.push_back(aeTick);
            newEvents.push_back(static_cast<uint32_t>(u2 & 0xFFFFFFFFu));
            newTicks.push_back(aeTick);
            break;
        }
        case PianoRollAutomationEvent::Type::PerNoteParameter: {
            const uint8_t noteNum = findNoteNum(ae);
            const auto d32 = static_cast<uint32_t>(std::round(v * 4294967295.0));
            const uint64_t u2 = umppi::UmpFactory::midi2PerNoteRCC(
                defaultGroup, ae.channel, noteNum,
                static_cast<uint8_t>(ae.paramIndex & 0xFFu), d32);
            newEvents.push_back(static_cast<uint32_t>(u2 >> 32));
            newTicks.push_back(aeTick);
            newEvents.push_back(static_cast<uint32_t>(u2 & 0xFFFFFFFFu));
            newTicks.push_back(aeTick);
            break;
        }
        default:
            break;
        }
        emittedClipAutomationWords.push_back(
            newEvents.size() > eventWord ? eventWord : SIZE_MAX);
    }

    std::vector<size_t> wordOrder;
    sortRawMidiEvents(newEvents, newTicks, wordOrder);
    std::vector<size_t> previewWordOffsets(newEvents.size(), SIZE_MAX);
    auto committedRaw = std::make_shared<PianoRollRawMidiData>();
    committedRaw->tickResolution = tickRes;
    committedRaw->clipTempo = bpm;
    for (size_t i = 0; i < newEvents.size();) {
        umppi::Ump first(newEvents[i]);
        const size_t messageWords = std::min(
            static_cast<size_t>(std::max(1, first.getSizeInInts())), newEvents.size() - i);
        umppi::Ump message = messageWords >= 2
            ? umppi::Ump(newEvents[i], newEvents[i + 1]) : first;
        previewWordOffsets[i] = committedRaw->umpEvents.size();
        std::vector<umppi::Ump> translated;
        std::vector<umppi::Ump> sourceMessage{message};
        umppi::UmpTranslator::translateMidi1UmpToMidi2Ump(translated, sourceMessage);
        for (const auto& ump : translated) {
            const auto output = ump.toWords();
            for (int word = 0; word < std::max(1, ump.getSizeInInts()); ++word) {
                committedRaw->umpEvents.push_back(output[static_cast<size_t>(word)]);
                committedRaw->tickTimestamps.push_back(newTicks[i]);
            }
        }
        i += messageWords;
    }
    std::unordered_map<uint64_t, size_t> emittedOnWords;
    for (const auto& [word, id] : emittedIds)
        emittedOnWords.emplace(id, previewWordOffsets[wordOrder[word]]);

    auto previewOffset = [&](size_t originalWord) {
        return originalWord < wordOrder.size()
            ? previewWordOffsets[wordOrder[originalWord]] : SIZE_MAX;
    };

    if (!app.sequencer().engine()->timeline().replaceMidiClipContent(
            trackIndex, clipId, std::move(newEvents), std::move(newTicks))) {
        edit_error = "Could not save note edits.";
        retry_available = true;
        return false;
    }
    edit_error.clear();
    retry_available = false;
    deletedRawIdxs.clear();

    // The writer already knows the committed event order. Update its raw indices
    // directly and keep the editable note objects, selection, and viewport intact.
    std::erase_if(editNotes, [](const PianoRollEditNote& note) { return note.deleted; });
    selectedNoteIdx = -1;
    std::unordered_set<uint64_t> liveIds;
    uint8_t minNote = 127, maxNote = 0;
    double noteEnd = 0.0;
    for (int i = 0; i < static_cast<int>(editNotes.size()); ++i) {
        auto& note = editNotes[i];
        liveIds.insert(note.edit_id);
        note.noteOnWordIdx = emittedOnWords.at(note.edit_id);
        note.noteOffWordIdx = previewOffset(emittedOffWords.at(note.edit_id));
        const auto& automationWords = emittedNoteAutomationWords[note.edit_id];
        for (size_t event = 0; event < note.automationEvents.size(); ++event)
            note.automationEvents[event].rawEventIdx = previewOffset(automationWords[event]);
        if (note.edit_id == primaryId)
            selectedNoteIdx = i;
        minNote = std::min(minNote, note.note);
        maxNote = std::max(maxNote, note.note);
        noteEnd = std::max(noteEnd, note.startSeconds + note.durationSeconds);
    }
    for (size_t event = 0; event < editClipEvents.size(); ++event)
        editClipEvents[event].rawEventIdx = previewOffset(emittedClipAutomationWords[event]);
    std::erase_if(selected_notes, [&](uint64_t id) { return !liveIds.contains(id); });
    this->minNote = editNotes.empty() ? 48 : minNote;
    this->maxNote = editNotes.empty() ? 72 : maxNote;
    rawMidiData = std::move(committedRaw);
    const auto tracks = app.getTimelineTracks();
    const auto* clip = trackIndex >= 0 && trackIndex < static_cast<int32_t>(tracks.size())
        && tracks[trackIndex] ? tracks[trackIndex]->clipManager().getClip(clipId) : nullptr;
    const double committedDuration = clip ? static_cast<double>(clip->durationSamples)
        / std::max(1.0, static_cast<double>(app.sampleRate())) : noteEnd;
    clipDurationSeconds = std::max(0.01, committedDuration);
    return true;
}


template<typename Timestamp>
void translateTimestampedMidi1UmpToMidi2(const std::vector<uapmd_ump_t>& sourceEvents,
                                         const std::vector<Timestamp>& sourceTimestamps,
                                         std::vector<uapmd_ump_t>& translatedEvents,
                                         std::vector<Timestamp>& translatedTimestamps) {
    translatedEvents.clear();
    translatedTimestamps.clear();
    translatedEvents.reserve(sourceEvents.size() * 2);
    translatedTimestamps.reserve(sourceTimestamps.size() * 2);

    const size_t eventCount = std::min(sourceEvents.size(), sourceTimestamps.size());
    size_t i = 0;
    while (i < eventCount) {
        umppi::Ump ump1(sourceEvents[i]);
        const int wordCount = std::max(1, ump1.getSizeInInts());
        const size_t safeCount = std::min(static_cast<size_t>(wordCount), eventCount - i);
        umppi::Ump ump = (safeCount >= 2) ? umppi::Ump(sourceEvents[i], sourceEvents[i + 1]) : ump1;
        const auto timestamp = sourceTimestamps[i];

        std::vector<umppi::Ump> translated;
        std::vector<umppi::Ump> sourceMessage{ump};
        umppi::UmpTranslator::translateMidi1UmpToMidi2Ump(translated, sourceMessage);
        for (const auto& translatedUmp : translated) {
            const int translatedWordCount = std::max(1, translatedUmp.getSizeInInts());
            const auto translatedWords = translatedUmp.toWords();
            for (int word = 0; word < translatedWordCount; ++word) {
                translatedEvents.push_back(translatedWords[static_cast<size_t>(word)]);
                translatedTimestamps.push_back(timestamp);
            }
        }

        i += static_cast<size_t>(wordCount);
    }
}


PianoRollClipSnapshot AppModel::pianoRollClipSnapshot(
    int32_t trackIndex,
    const uapmd::ClipData& clipData,
    double fallbackDurationSeconds
) {
    PianoRollClipSnapshot snapshot;

    double durationSeconds = static_cast<double>(clipData.durationSamples) /
        std::max(1.0, static_cast<double>(sampleRate()));
    if (durationSeconds <= 0.0) {
        durationSeconds = fallbackDurationSeconds;
    }
    snapshot.durationSeconds = std::max(0.01, durationSeconds);

    auto tracks = getTimelineTracks();
    if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(tracks.size())) {
        snapshot.ready = false;
        snapshot.error = "Track unavailable";
        return snapshot;
    }

    auto* track = tracks[trackIndex];
    if (!track) {
        snapshot.ready = false;
        snapshot.error = "Track unavailable";
        return snapshot;
    }

    auto sourceNode = track->getSourceNode(clipData.sourceNodeInstanceId);
    auto* midiSource = dynamic_cast<uapmd::MidiClipSourceNode*>(sourceNode.get());
    if (!midiSource) {
        snapshot.ready = false;
        snapshot.error = "Missing MIDI source";
        return snapshot;
    }

    std::vector<uapmd_ump_t> normalizedTickEvents;
    std::vector<uint64_t> normalizedTickTimestamps;
    translateTimestampedMidi1UmpToMidi2(
        midiSource->umpEvents(), midiSource->eventTimestampsTicks(),
        normalizedTickEvents, normalizedTickTimestamps);

    std::vector<uapmd_ump_t> normalizedSampleEvents;
    std::vector<uint64_t> normalizedSampleTimestamps;
    translateTimestampedMidi1UmpToMidi2(
        midiSource->umpEvents(), midiSource->eventTimestampsSamples(),
        normalizedSampleEvents, normalizedSampleTimestamps);

    // Capture normalized UMP data for piano-roll write-back before parsing into notes.
    auto rawData = std::make_shared<PianoRollRawMidiData>();
    rawData->umpEvents       = normalizedTickEvents;
    rawData->tickTimestamps  = normalizedTickTimestamps;
    rawData->tickResolution  = clipData.tickResolution > 0
                               ? clipData.tickResolution
                               : midiSource->tickResolution();
    rawData->clipTempo       = midiSource->clipTempo();
    snapshot.rawMidiData = rawData;

    const auto& events = normalizedSampleEvents;
    const auto& timestamps = normalizedSampleTimestamps;
    if (events.empty() || timestamps.empty()) {
        snapshot.ready = true;
        return snapshot;
    }

    const double safeSampleRate = std::max(1.0, static_cast<double>(sampleRate()));
    const size_t eventCount = std::min(events.size(), timestamps.size());
    // Maps (group<<12|channel<<7|note) -> index in snapshot.notes for in-flight notes.
    std::unordered_map<uint32_t, size_t> activeNoteIndices;
    activeNoteIndices.reserve(64);

    // Iterate messages, advancing by getSizeInInts() to handle multi-word MIDI2 messages.
    // NoteOn/NoteOff are processed here. The session loads automation from rawMidiData.
    size_t i = 0;
    while (i < eventCount) {
        umppi::Ump ump1(events[i]);
        const int wordCount = ump1.getSizeInInts();
        const size_t safeCount = std::min(static_cast<size_t>(wordCount), eventCount - i);
        umppi::Ump ump = (safeCount >= 2) ? umppi::Ump(events[i], events[i + 1]) : ump1;

        const double eventSeconds = static_cast<double>(timestamps[i]) / safeSampleRate;
        const auto msgType = ump.getMessageType();

        if (msgType == umppi::MessageType::MIDI2) {
            const uint8_t status = ump.getStatusCode();
            const uint8_t channel = ump.getChannelInGroup();
            const uint8_t group = ump.getGroup();

            if (status == umppi::MidiChannelStatus::NOTE_ON || status == umppi::MidiChannelStatus::NOTE_OFF) {
                const uint8_t  noteNum = ump.getMidi2Note();
                const uint16_t vel16   = ump.getMidi2Velocity16();
                const uint32_t key = (static_cast<uint32_t>(group) << 12) |
                                     (static_cast<uint32_t>(channel) << 7) | noteNum;
                const bool isNoteOn = (status == umppi::MidiChannelStatus::NOTE_ON) && vel16 > 0;
                if (isNoteOn) {
                    PianoRollMidiNote note{};
                    note.startSeconds  = eventSeconds;
                    note.note          = noteNum;
                    note.velocity      = vel16 / 65535.0f;
                    note.channel       = channel;
                    note.noteOnWordIdx = i;
                    activeNoteIndices[key] = snapshot.notes.size();
                    snapshot.notes.push_back(std::move(note));
                } else {
                    auto it = activeNoteIndices.find(key);
                    if (it != activeNoteIndices.end()) {
                        auto& n = snapshot.notes[it->second];
                        n.durationSeconds = std::max(0.01, eventSeconds - n.startSeconds);
                        n.noteOffWordIdx  = i;
                        activeNoteIndices.erase(it);
                    }
                }
            }
        }

        i += static_cast<size_t>(std::max(1, wordCount));
    }

    // Finalize any notes that had no matching NoteOff.
    for (auto& [key, idx] : activeNoteIndices) {
        auto& n = snapshot.notes[idx];
        n.durationSeconds = std::max(0.01, snapshot.durationSeconds - n.startSeconds);
    }

    uint8_t minNote = 127;
    uint8_t maxNote = 0;
    for (const auto& note : snapshot.notes) {
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
