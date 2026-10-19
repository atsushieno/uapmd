#include "uapmd-data/uapmd-data.hpp"

#include <algorithm>
#include <atomic>
#include <unordered_map>
#include <umppi/umppi.hpp>

namespace uapmd {
    namespace {
        std::atomic<uint64_t> next_item_id{1};
        ProjectObjectId allocateItemId() {
            return "midi-item:" + std::to_string(next_item_id.fetch_add(1, std::memory_order_relaxed));
        }
    }

    MidiRawData MidiClipDocument::normalize(MidiRawData raw) {
        MidiRawData result;
        result.tickResolution = raw.tickResolution;
        result.clipTempo = raw.clipTempo;
        for (size_t i = 0; i < raw.umpEvents.size();) {
            umppi::Ump first(raw.umpEvents[i]);
            const auto count = static_cast<size_t>(std::max(1, first.getSizeInInts()));
            if (i + count > raw.umpEvents.size())
                break;
            // The translator handles MIDI channel messages (32/64 bits). Preserve
            // wider messages verbatim so editing notes does not truncate SysEx/data.
            if (count > 2) {
                for (size_t word = 0; word < count; ++word) {
                    result.umpEvents.push_back(raw.umpEvents[i + word]);
                    result.tickTimestamps.push_back(i < raw.tickTimestamps.size() ? raw.tickTimestamps[i] : 0);
                }
            } else {
                auto message = count == 2 ? umppi::Ump(raw.umpEvents[i], raw.umpEvents[i + 1]) : first;
                std::vector<umppi::Ump> translated;
                umppi::UmpTranslator::translateMidi1UmpToMidi2Ump(translated, std::vector<umppi::Ump>{message});
                for (const auto& output : translated) {
                    const auto words = output.toWords();
                    for (int word = 0; word < std::max(1, output.getSizeInInts()); ++word) {
                        result.umpEvents.push_back(words[static_cast<size_t>(word)]);
                        result.tickTimestamps.push_back(i < raw.tickTimestamps.size() ? raw.tickTimestamps[i] : 0);
                    }
                }
            }
            i += count;
        }
        return result;
    }

    bool MidiClipDocument::matchesSource(const MidiRawData& incoming) const {
        const auto normalized = normalize(incoming);
        return committed_.raw.umpEvents == normalized.umpEvents &&
            committed_.raw.tickTimestamps == normalized.tickTimestamps &&
            committed_.raw.tickResolution == normalized.tickResolution &&
            committed_.raw.clipTempo == normalized.clipTempo;
    }

    void MidiClipDocument::ensureIdentities() {
        auto identify = [](MidiAutomationEvent& event) {
            if (event.reference_id.empty())
                event.reference_id = allocateItemId();
        };
        for (auto& note : editNotes) {
            if (note.reference_id.empty())
                note.reference_id = allocateItemId();
            if (!note.edit_id)
                note.edit_id = next_note_id++;
            next_note_id = std::max(next_note_id, note.edit_id + 1);
            for (auto& event : note.automationEvents)
                identify(event);
        }
        for (auto& event : editClipEvents)
            identify(event);
    }

    MidiClipContentSnapshot MidiClipDocument::snapshot() const {
        return {rawMidiData ? *rawMidiData : MidiRawData{}, editNotes, editClipEvents};
    }

    void MidiClipDocument::markCommitted() {
        ensureIdentities();
        committed_ = snapshot();
    }

    void MidiClipDocument::restore(MidiClipContentSnapshot snapshot, bool mintIdentities) {
        rawMidiData = std::make_shared<MidiRawData>(std::move(snapshot.raw));
        editNotes = std::move(snapshot.notes);
        editClipEvents = std::move(snapshot.automation);
        if (mintIdentities) {
            for (auto& note : editNotes) {
                note.reference_id.clear();
                note.edit_id = 0;
                for (auto& event : note.automationEvents)
                    event.reference_id.clear();
            }
            for (auto& event : editClipEvents)
                event.reference_id.clear();
        }
        markCommitted();
    }

    std::optional<MidiClipContentSnapshot> MidiClipDocument::withAppendedEvents(MidiRawData incoming) const {
        if (incoming.umpEvents.size() != incoming.tickTimestamps.size())
            return std::nullopt;
        for (size_t i = 0; i < incoming.umpEvents.size();) {
            const auto count = static_cast<size_t>(std::max(1, umppi::Ump(incoming.umpEvents[i]).getSizeInInts()));
            if (i + count > incoming.umpEvents.size())
                return std::nullopt;
            i += count;
        }
        incoming = normalize(std::move(incoming));
        struct Message { uint64_t tick; std::vector<uapmd_ump_t> words; std::optional<size_t> original; };
        std::vector<Message> messages;
        auto append = [&](const MidiRawData& raw, bool original) {
            for (size_t i = 0; i < raw.umpEvents.size();) {
                const auto count = static_cast<size_t>(std::max(1, umppi::Ump(raw.umpEvents[i]).getSizeInInts()));
                messages.push_back({raw.tickTimestamps[i], {raw.umpEvents.begin() + i, raw.umpEvents.begin() + i + count},
                    original ? std::optional<size_t>(i) : std::nullopt});
                i += count;
            }
        };
        append(committed_.raw, true);
        append(incoming, false);
        std::stable_sort(messages.begin(), messages.end(), [](const auto& a, const auto& b) { return a.tick < b.tick; });
        MidiRawData merged;
        merged.tickResolution = committed_.raw.tickResolution;
        merged.clipTempo = committed_.raw.clipTempo;
        std::unordered_map<size_t, size_t> originalOffsets;
        for (const auto& message : messages) {
            if (message.original)
                originalOffsets[merged.umpEvents.size()] = *message.original;
            merged.umpEvents.insert(merged.umpEvents.end(), message.words.begin(), message.words.end());
            merged.tickTimestamps.insert(merged.tickTimestamps.end(), message.words.size(), message.tick);
        }
        std::unordered_map<size_t, const MidiEditNote*> oldNotes;
        std::unordered_map<size_t, ProjectObjectId> oldPoints;
        for (const auto& note : committed_.notes) {
            oldNotes[note.noteOnWordIdx] = &note;
            for (const auto& point : note.automationEvents)
                oldPoints[point.rawEventIdx] = point.reference_id;
        }
        for (const auto& point : committed_.automation)
            oldPoints[point.rawEventIdx] = point.reference_id;
        MidiClipDocument candidate;
        candidate.load(std::move(merged), 0.01);
        candidate.next_note_id = next_note_id;
        auto preservePoint = [&](MidiAutomationEvent& point) {
            if (auto offset = originalOffsets.find(point.rawEventIdx); offset != originalOffsets.end())
                if (auto id = oldPoints.find(offset->second); id != oldPoints.end())
                    point.reference_id = id->second;
        };
        for (auto& note : candidate.editNotes) {
            bool preserved = false;
            if (auto offset = originalOffsets.find(note.noteOnWordIdx); offset != originalOffsets.end())
                if (auto old = oldNotes.find(offset->second); old != oldNotes.end()) {
                    note.reference_id = old->second->reference_id;
                    note.edit_id = old->second->edit_id;
                    preserved = true;
                }
            if (!preserved)
                note.edit_id = candidate.next_note_id++;
            for (auto& point : note.automationEvents)
                preservePoint(point);
        }
        for (auto& point : candidate.editClipEvents)
            preservePoint(point);
        return candidate.snapshot();
    }

    void MidiClipDocument::load(MidiRawData raw, double durationSeconds) {
        rawMidiData = std::make_shared<MidiRawData>(normalize(std::move(raw)));
        editNotes.clear();
        editClipEvents.clear();
        std::unordered_map<uint32_t, size_t> active;
        const double secondsPerTick = 60.0 /
            (std::max(1.0, rawMidiData->clipTempo) * std::max<uint32_t>(1, rawMidiData->tickResolution));
        for (size_t i = 0; i < rawMidiData->umpEvents.size();) {
            umppi::Ump first(rawMidiData->umpEvents[i]);
            const auto count = static_cast<size_t>(std::max(1, first.getSizeInInts()));
            if (i + count > rawMidiData->umpEvents.size())
                break;
            if (count == 2 && first.getMessageType() == umppi::MessageType::MIDI2) {
                umppi::Ump message(rawMidiData->umpEvents[i], rawMidiData->umpEvents[i + 1]);
                const auto status = message.getStatusCode();
                if (status == umppi::MidiChannelStatus::NOTE_ON || status == umppi::MidiChannelStatus::NOTE_OFF) {
                    const uint32_t key = (message.getGroup() << 12) | (message.getChannelInGroup() << 7) | message.getMidi2Note();
                    const double seconds = (i < rawMidiData->tickTimestamps.size() ? rawMidiData->tickTimestamps[i] : 0) * secondsPerTick;
                    if (status == umppi::MidiChannelStatus::NOTE_ON && message.getMidi2Velocity16()) {
                        MidiEditNote note;
                        note.startSeconds = seconds;
                        note.durationSeconds = std::max(0.01, durationSeconds - seconds);
                        note.note = message.getMidi2Note();
                        note.velocity = message.getMidi2Velocity16() / 65535.0f;
                        note.channel = message.getChannelInGroup();
                        note.noteOnWordIdx = i;
                        active[key] = editNotes.size();
                        editNotes.push_back(std::move(note));
                    } else if (auto it = active.find(key); it != active.end()) {
                        auto& note = editNotes[it->second];
                        note.durationSeconds = std::max(0.01, seconds - note.startSeconds);
                        note.noteOffWordIdx = i;
                        active.erase(it);
                    }
                }
            }
            i += count;
        }
        seedNoteAttributesFromRaw(*rawMidiData, editNotes);
        parseAutomationFromRaw(*rawMidiData, editNotes, editClipEvents);
        markCommitted();
    }

void MidiClipDocument::parseAutomationFromRaw(
        const MidiRawData& raw,
        std::vector<MidiEditNote>&                      editNotes,
        std::vector<MidiAutomationEvent>&  clipEvents) {
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

    auto addPerNoteEvt = [&](uint32_t noteKey, MidiAutomationEvent evt) {
        auto it = activeNoteIndices.find(noteKey);
        if (it != activeNoteIndices.end())
            editNotes[it->second].automationEvents.push_back(std::move(evt));
        else
            clipEvents.push_back(std::move(evt));
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
                MidiAutomationEvent evt{};
                evt.timeSeconds     = t;
                evt.normalizedValue = ump.getMidi1Lsb() / 127.0;
                evt.type            = MidiAutomationEvent::Type::PolyPressure;
                evt.channel         = channel;
                evt.umpGroup        = group;
                evt.noteNumber      = note;
                evt.rawEventIdx     = i;
                addPerNoteEvt(baseKey | note, evt);
            } else if (status == umppi::MidiChannelStatus::CC) {
                MidiAutomationEvent evt{};
                evt.timeSeconds     = t;
                evt.normalizedValue = ump.getMidi1CCData() / 127.0;
                evt.type            = MidiAutomationEvent::Type::ControlChange;
                evt.channel         = channel;
                evt.umpGroup        = group;
                evt.paramIndex      = ump.getMidi1CCIndex();
                evt.rawEventIdx     = i;
                clipEvents.push_back(evt);
            } else if (status == umppi::MidiChannelStatus::CAF) {
                MidiAutomationEvent evt{};
                evt.timeSeconds     = t;
                evt.normalizedValue = ump.getMidi1Msb() / 127.0;
                evt.type            = MidiAutomationEvent::Type::ChannelPressure;
                evt.channel         = channel;
                evt.umpGroup        = group;
                evt.rawEventIdx     = i;
                clipEvents.push_back(evt);
            } else if (status == umppi::MidiChannelStatus::PITCH_BEND) {
                MidiAutomationEvent evt{};
                evt.timeSeconds     = t;
                evt.normalizedValue = ump.getMidi1PitchBendData() / 16383.0;
                evt.type            = MidiAutomationEvent::Type::PitchBend;
                evt.channel         = channel;
                evt.umpGroup        = group;
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
                MidiAutomationEvent evt{};
                evt.timeSeconds     = t;
                evt.normalizedValue = ump.getMidi2PafData() / static_cast<double>(0xFFFFFFFFu);
                evt.type            = MidiAutomationEvent::Type::PolyPressure;
                evt.channel         = channel;
                evt.umpGroup        = group;
                evt.noteNumber      = note;
                evt.rawEventIdx     = i;
                addPerNoteEvt(baseKey | note, evt);
            } else if (status == umppi::MidiChannelStatus::PER_NOTE_PITCH_BEND) {
                const uint8_t note = ump.getMidi2Note();
                MidiAutomationEvent evt{};
                evt.timeSeconds     = t;
                evt.normalizedValue = ump.getMidi2PitchBendData() / static_cast<double>(0xFFFFFFFFu);
                evt.type            = MidiAutomationEvent::Type::PerNotePitchBend;
                evt.channel         = channel;
                evt.umpGroup        = group;
                evt.noteNumber      = note;
                evt.rawEventIdx     = i;
                addPerNoteEvt(baseKey | note, evt);
            } else if (status == umppi::MidiChannelStatus::PER_NOTE_RCC ||
                       status == umppi::MidiChannelStatus::PER_NOTE_ACC) {
                const uint8_t note = ump.getMidi2Note();
                MidiAutomationEvent evt{};
                evt.timeSeconds     = t;
                evt.normalizedValue = ump.getMidi2CcData() / static_cast<double>(0xFFFFFFFFu);
                evt.type            = MidiAutomationEvent::Type::PerNoteParameter;
                evt.registered_per_note = status == umppi::MidiChannelStatus::PER_NOTE_RCC;
                evt.channel         = channel;
                evt.umpGroup        = group;
                evt.noteNumber      = note;
                evt.paramIndex      = static_cast<uint16_t>(events[i] & 0xFF);
                evt.rawEventIdx     = i;
                addPerNoteEvt(baseKey | note, evt);
            } else if (status == umppi::MidiChannelStatus::CC) {
                MidiAutomationEvent evt{};
                evt.timeSeconds     = t;
                evt.normalizedValue = ump.getMidi2CcData() / static_cast<double>(0xFFFFFFFFu);
                evt.type            = MidiAutomationEvent::Type::ControlChange;
                evt.channel         = channel;
                evt.umpGroup        = group;
                evt.paramIndex      = ump.getMidi2CcIndex();
                evt.rawEventIdx     = i;
                clipEvents.push_back(evt);
            } else if (status == umppi::MidiChannelStatus::RPN) {
                MidiAutomationEvent evt{};
                evt.timeSeconds     = t;
                evt.normalizedValue = ump.getMidi2RpnData() / static_cast<double>(0xFFFFFFFFu);
                evt.type            = MidiAutomationEvent::Type::RPN;
                evt.channel         = channel;
                evt.umpGroup        = group;
                evt.paramIndex      = static_cast<uint16_t>((ump.getMidi2RpnMsb() << 7) | ump.getMidi2RpnLsb());
                evt.rawEventIdx     = i;
                clipEvents.push_back(evt);
            } else if (status == umppi::MidiChannelStatus::NRPN) {
                MidiAutomationEvent evt{};
                evt.timeSeconds     = t;
                evt.normalizedValue = ump.getMidi2NrpnData() / static_cast<double>(0xFFFFFFFFu);
                evt.type            = MidiAutomationEvent::Type::NRPN;
                evt.channel         = channel;
                evt.umpGroup        = group;
                evt.umpGroup        = group;
                evt.paramIndex      = static_cast<uint16_t>((ump.getMidi2NrpnMsb() << 7) | ump.getMidi2NrpnLsb());
                evt.rawEventIdx     = i;
                clipEvents.push_back(evt);
            } else if (status == umppi::MidiChannelStatus::CAF) {
                MidiAutomationEvent evt{};
                evt.timeSeconds     = t;
                evt.normalizedValue = ump.getMidi2CafData() / static_cast<double>(0xFFFFFFFFu);
                evt.type            = MidiAutomationEvent::Type::ChannelPressure;
                evt.channel         = channel;
                evt.umpGroup        = group;
                evt.rawEventIdx     = i;
                clipEvents.push_back(evt);
            } else if (status == umppi::MidiChannelStatus::PITCH_BEND) {
                MidiAutomationEvent evt{};
                evt.timeSeconds     = t;
                evt.normalizedValue = ump.getMidi2PitchBendData() / static_cast<double>(0xFFFFFFFFu);
                evt.type            = MidiAutomationEvent::Type::PitchBend;
                evt.channel         = channel;
                evt.umpGroup        = group;
                evt.rawEventIdx     = i;
                clipEvents.push_back(evt);
            }
        }

        i += static_cast<size_t>(std::max(1, wordCount));
    }
}

void MidiClipDocument::seedNoteAttributesFromRaw(const MidiRawData& raw,
                                                std::vector<MidiEditNote>& editNotes) {
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


MidiClipContentSnapshot MidiClipDocument::encodedSnapshot(const std::vector<size_t>& deletedRawIdxs) {
    ensureIdentities();
    if (!rawMidiData)
        return {};
    // Encode a candidate so a failed engine commit leaves the editable state intact.
    auto editNotes = this->editNotes;
    auto editClipEvents = this->editClipEvents;
    const MidiRawData& orig = *rawMidiData;
    const uint32_t tickRes = orig.tickResolution;
    const double   bpm     = orig.clipTempo > 0.0 ? orig.clipTempo : 120.0;

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
            case MidiAutomationEvent::Type::PitchBend: {
                const auto d14 = static_cast<uint16_t>(std::round(v * 16383.0));
                newEvents.push_back(umppi::UmpFactory::midi1PitchBendDirect(grp, ch, d14));
                newTicks.push_back(aeTick);
                break;
            }
            case MidiAutomationEvent::Type::PerNotePitchBend: {
                const auto d32 = static_cast<uint32_t>(std::round(v * 4294967295.0));
                const uint64_t u2 = umppi::UmpFactory::midi2PerNotePitchBendDirect(
                    grp, ch, editNote.note, d32);
                newEvents.push_back(static_cast<uint32_t>(u2 >> 32));
                newTicks.push_back(aeTick);
                newEvents.push_back(static_cast<uint32_t>(u2 & 0xFFFFFFFFu));
                newTicks.push_back(aeTick);
                break;
            }
            case MidiAutomationEvent::Type::ChannelPressure: {
                const auto d7 = static_cast<uint8_t>(std::round(v * 127.0));
                newEvents.push_back(umppi::UmpFactory::midi1CAf(grp, ch, d7));
                newTicks.push_back(aeTick);
                break;
            }
            case MidiAutomationEvent::Type::PolyPressure: {
                const auto d7 = static_cast<uint8_t>(std::round(v * 127.0));
                newEvents.push_back(umppi::UmpFactory::midi1PAf(grp, ch, editNote.note, d7));
                newTicks.push_back(aeTick);
                break;
            }
            case MidiAutomationEvent::Type::ControlChange: {
                const auto cc  = static_cast<uint8_t>(ae.paramIndex & 0x7Fu);
                const auto d32 = static_cast<uint32_t>(std::round(v * 4294967295.0));
                const uint64_t u2 = umppi::UmpFactory::midi2CC(grp, ch, cc, d32);
                newEvents.push_back(static_cast<uint32_t>(u2 >> 32));
                newTicks.push_back(aeTick);
                newEvents.push_back(static_cast<uint32_t>(u2 & 0xFFFFFFFFu));
                newTicks.push_back(aeTick);
                break;
            }
            case MidiAutomationEvent::Type::RPN: {
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
            case MidiAutomationEvent::Type::NRPN: {
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
            case MidiAutomationEvent::Type::PerNoteParameter: {
                const auto d32 = static_cast<uint32_t>(std::round(v * 4294967295.0));
                const uint64_t u2 = ae.registered_per_note
                    ? umppi::UmpFactory::midi2PerNoteRCC(grp, ch, editNote.note, static_cast<uint8_t>(ae.paramIndex & 0xFFu), d32)
                    : umppi::UmpFactory::midi2PerNoteACC(grp, ch, editNote.note, static_cast<uint8_t>(ae.paramIndex & 0xFFu), d32);
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

    // Clip-owned per-note messages retain their explicit note address, including
    // note zero; ownership is not inferred from overlapping note geometry.
    for (const auto& ae : editClipEvents) {
        const size_t eventWord = newEvents.size();
        const uint64_t aeTick = secondsToTicks(ae.timeSeconds, tickRes, bpm);
        const double   v      = std::clamp(ae.normalizedValue, 0.0, 1.0);
        // Preserve each point's group and channel.
        switch (ae.type) {
        case MidiAutomationEvent::Type::PitchBend: {
            const auto d14 = static_cast<uint16_t>(std::round(v * 16383.0));
            newEvents.push_back(umppi::UmpFactory::midi1PitchBendDirect(ae.umpGroup, ae.channel, d14));
            newTicks.push_back(aeTick);
            break;
        }
        case MidiAutomationEvent::Type::ChannelPressure: {
            const auto d7 = static_cast<uint8_t>(std::round(v * 127.0));
            newEvents.push_back(umppi::UmpFactory::midi1CAf(ae.umpGroup, ae.channel, d7));
            newTicks.push_back(aeTick);
            break;
        }
        case MidiAutomationEvent::Type::ControlChange: {
            const auto cc  = static_cast<uint8_t>(ae.paramIndex & 0x7Fu);
            const auto d32 = static_cast<uint32_t>(std::round(v * 4294967295.0));
            const uint64_t u2 = umppi::UmpFactory::midi2CC(ae.umpGroup, ae.channel, cc, d32);
            newEvents.push_back(static_cast<uint32_t>(u2 >> 32));
            newTicks.push_back(aeTick);
            newEvents.push_back(static_cast<uint32_t>(u2 & 0xFFFFFFFFu));
            newTicks.push_back(aeTick);
            break;
        }
        case MidiAutomationEvent::Type::RPN: {
            const auto msb = static_cast<uint8_t>(ae.paramIndex >> 7);
            const auto lsb = static_cast<uint8_t>(ae.paramIndex & 0x7Fu);
            const auto d32 = static_cast<uint32_t>(std::round(v * 4294967295.0));
            const uint64_t u2 = umppi::UmpFactory::midi2RPN(ae.umpGroup, ae.channel, msb, lsb, d32);
            newEvents.push_back(static_cast<uint32_t>(u2 >> 32));
            newTicks.push_back(aeTick);
            newEvents.push_back(static_cast<uint32_t>(u2 & 0xFFFFFFFFu));
            newTicks.push_back(aeTick);
            break;
        }
        case MidiAutomationEvent::Type::NRPN: {
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
        case MidiAutomationEvent::Type::PolyPressure: {
            const uint8_t noteNum = ae.noteNumber;
            const auto d7 = static_cast<uint8_t>(std::round(v * 127.0));
            newEvents.push_back(umppi::UmpFactory::midi1PAf(ae.umpGroup, ae.channel, noteNum, d7));
            newTicks.push_back(aeTick);
            break;
        }
        case MidiAutomationEvent::Type::PerNotePitchBend: {
            const uint8_t noteNum = ae.noteNumber;
            const auto d32 = static_cast<uint32_t>(std::round(v * 4294967295.0));
            const uint64_t u2 = umppi::UmpFactory::midi2PerNotePitchBendDirect(
                ae.umpGroup, ae.channel, noteNum, d32);
            newEvents.push_back(static_cast<uint32_t>(u2 >> 32));
            newTicks.push_back(aeTick);
            newEvents.push_back(static_cast<uint32_t>(u2 & 0xFFFFFFFFu));
            newTicks.push_back(aeTick);
            break;
        }
        case MidiAutomationEvent::Type::PerNoteParameter: {
            const uint8_t noteNum = ae.noteNumber;
            const auto d32 = static_cast<uint32_t>(std::round(v * 4294967295.0));
            const uint64_t u2 = ae.registered_per_note
                ? umppi::UmpFactory::midi2PerNoteRCC(ae.umpGroup, ae.channel, noteNum, static_cast<uint8_t>(ae.paramIndex & 0xFFu), d32)
                : umppi::UmpFactory::midi2PerNoteACC(ae.umpGroup, ae.channel, noteNum, static_cast<uint8_t>(ae.paramIndex & 0xFFu), d32);
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
    auto committedRaw = std::make_shared<MidiRawData>();
    committedRaw->tickResolution = tickRes;
    committedRaw->clipTempo = bpm;
    for (size_t i = 0; i < newEvents.size();) {
        umppi::Ump first(newEvents[i]);
        const size_t messageWords = std::min(
            static_cast<size_t>(std::max(1, first.getSizeInInts())), newEvents.size() - i);
        if (messageWords > 2) {
            previewWordOffsets[i] = committedRaw->umpEvents.size();
            for (size_t word = 0; word < messageWords; ++word) {
                committedRaw->umpEvents.push_back(newEvents[i + word]);
                committedRaw->tickTimestamps.push_back(newTicks[i]);
            }
            i += messageWords;
            continue;
        }
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

    // The writer already knows the committed event order. Update its raw indices
    // directly and keep the editable note objects, selection, and viewport intact.
    std::erase_if(editNotes, [](const MidiEditNote& note) { return note.deleted; });
    for (int i = 0; i < static_cast<int>(editNotes.size()); ++i) {
        auto& note = editNotes[i];
        note.noteOnWordIdx = emittedOnWords.at(note.edit_id);
        note.noteOffWordIdx = previewOffset(emittedOffWords.at(note.edit_id));
        const auto& automationWords = emittedNoteAutomationWords[note.edit_id];
        for (size_t event = 0; event < note.automationEvents.size(); ++event)
            note.automationEvents[event].rawEventIdx = previewOffset(automationWords[event]);
    }
    for (size_t event = 0; event < editClipEvents.size(); ++event)
        editClipEvents[event].rawEventIdx = previewOffset(emittedClipAutomationWords[event]);
    return {*committedRaw, std::move(editNotes), std::move(editClipEvents)};
}

}
