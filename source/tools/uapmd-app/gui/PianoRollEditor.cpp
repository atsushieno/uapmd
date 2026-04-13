#include "PianoRollEditor.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>
#include <unordered_map>

#include "ContextActions.hpp"
#include "FontIcons.hpp"

#include <imgui.h>
#include <umppi/umppi.hpp>

namespace uapmd::gui {

namespace {

ImVec4 withAlpha(const ImVec4& color, float alpha) {
    return ImVec4(color.x, color.y, color.z, alpha);
}

ImVec4 mixColor(const ImVec4& a, const ImVec4& b, float t) {
    return ImLerp(a, b, t);
}

struct PianoRollTheme {
    ImU32 key_panel_bg;
    ImU32 key_ruler_bg;
    ImU32 key_black;
    ImU32 key_white;
    ImU32 key_preview_black;
    ImU32 key_preview_white;
    ImU32 key_label;
    ImU32 key_separator;
    ImU32 key_border;
    ImU32 editor_bg;
    ImU32 lane_black;
    ImU32 lane_white;
    ImU32 lane_octave;
    ImU32 lane_separator;
    ImU32 bar_line;
    ImU32 beat_line;
    ImU32 note_selected_fill;
    ImU32 note_fill_low;
    ImU32 note_fill_high;
    ImU32 note_selected_border;
    ImU32 note_border;
    ImU32 automation_dot;
    ImU32 ruler_bg;
    ImU32 ruler_line;
    ImU32 ruler_text;
    ImU32 ruler_bottom_line;
    ImU32 automation_related_text;
};

PianoRollTheme getPianoRollTheme() {
    const auto& style = ImGui::GetStyle();
    const auto& c = style.Colors;
    const bool dark = c[ImGuiCol_WindowBg].x + c[ImGuiCol_WindowBg].y + c[ImGuiCol_WindowBg].z < 1.5f;

    const ImVec4 window_bg = c[ImGuiCol_WindowBg];
    const ImVec4 child_bg = c[ImGuiCol_ChildBg].w > 0.0f ? c[ImGuiCol_ChildBg] : window_bg;
    const ImVec4 frame_bg = c[ImGuiCol_FrameBg];
    const ImVec4 border = c[ImGuiCol_Border];
    const ImVec4 text = c[ImGuiCol_Text];
    const ImVec4 text_disabled = c[ImGuiCol_TextDisabled];
    const ImVec4 accent = c[ImGuiCol_Button];
    const ImVec4 accent_hover = c[ImGuiCol_ButtonHovered];
    const ImVec4 accent_active = c[ImGuiCol_ButtonActive];
    const ImVec4 header = c[ImGuiCol_Header];

    const ImVec4 editor_bg = mixColor(child_bg, frame_bg, dark ? 0.22f : 0.45f);
    const ImVec4 lane_white = mixColor(editor_bg, text, dark ? 0.03f : 0.05f);
    const ImVec4 lane_black = mixColor(editor_bg, text, dark ? 0.08f : 0.10f);
    const ImVec4 lane_octave = mixColor(header, lane_white, dark ? 0.18f : 0.30f);
    const ImVec4 ruler_bg = mixColor(frame_bg, header, dark ? 0.30f : 0.18f);
    const ImVec4 note_fill_low = mixColor(editor_bg, accent, dark ? 0.50f : 0.30f);
    const ImVec4 note_fill_high = mixColor(accent_hover, accent_active, 0.45f);
    const ImVec4 selected_fill = mixColor(accent_active, header, 0.35f);

    PianoRollTheme theme{};
    theme.key_panel_bg = ImGui::GetColorU32(mixColor(window_bg, frame_bg, dark ? 0.35f : 0.18f));
    theme.key_ruler_bg = ImGui::GetColorU32(ruler_bg);
    theme.key_black = IM_COL32(25, 25, 28, 255);
    theme.key_white = IM_COL32(218, 218, 218, 255);
    theme.key_preview_black = ImGui::GetColorU32(mixColor(accent_active, ImVec4(0.0f, 0.0f, 0.0f, 1.0f), 0.20f));
    theme.key_preview_white = ImGui::GetColorU32(mixColor(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), accent_hover, 0.35f));
    theme.key_label = ImGui::GetColorU32(withAlpha(text, dark ? 0.58f : 0.70f));
    theme.key_separator = ImGui::GetColorU32(withAlpha(border, dark ? 0.55f : 0.45f));
    theme.key_border = ImGui::GetColorU32(withAlpha(border, dark ? 0.85f : 0.75f));
    theme.editor_bg = ImGui::GetColorU32(editor_bg);
    theme.lane_black = ImGui::GetColorU32(lane_black);
    theme.lane_white = ImGui::GetColorU32(lane_white);
    theme.lane_octave = ImGui::GetColorU32(lane_octave);
    theme.lane_separator = ImGui::GetColorU32(withAlpha(border, dark ? 0.28f : 0.22f));
    theme.bar_line = ImGui::GetColorU32(withAlpha(text, dark ? 0.22f : 0.12f));
    theme.beat_line = ImGui::GetColorU32(withAlpha(text_disabled, dark ? 0.28f : 0.18f));
    theme.note_selected_fill = ImGui::GetColorU32(withAlpha(selected_fill, 0.95f));
    theme.note_fill_low = ImGui::GetColorU32(withAlpha(note_fill_low, 0.88f));
    theme.note_fill_high = ImGui::GetColorU32(withAlpha(note_fill_high, 0.95f));
    theme.note_selected_border = ImGui::GetColorU32(withAlpha(accent_active, 0.95f));
    theme.note_border = ImGui::GetColorU32(withAlpha(border, dark ? 0.72f : 0.60f));
    theme.automation_dot = ImGui::GetColorU32(withAlpha(mixColor(accent_hover, accent_active, 0.5f), 0.96f));
    theme.ruler_bg = ImGui::GetColorU32(withAlpha(ruler_bg, dark ? 0.96f : 0.92f));
    theme.ruler_line = ImGui::GetColorU32(withAlpha(border, dark ? 0.85f : 0.65f));
    theme.ruler_text = ImGui::GetColorU32(withAlpha(text, dark ? 0.78f : 0.62f));
    theme.ruler_bottom_line = ImGui::GetColorU32(withAlpha(border, dark ? 0.88f : 0.72f));
    theme.automation_related_text = ImGui::GetColorU32(withAlpha(mixColor(text, accent_hover, 0.45f), 1.0f));
    return theme;
}

} // namespace

// ── static helpers ──────────────────────────────────────────────────────────

bool PianoRollEditor::isBlackKey(int midiNote) noexcept {
    int n = midiNote % 12;
    return n == 1 || n == 3 || n == 6 || n == 8 || n == 10;
}

const char* PianoRollEditor::noteNameCStr(int midiNote) noexcept {
    static const char* const kNames[12] = {
        "C","C#","D","D#","E","F","F#","G","G#","A","A#","B"
    };
    return kNames[midiNote % 12];
}

std::string PianoRollEditor::fullNoteName(int midiNote) {
    int octave = midiNote / 12 - 1; // MIDI 60 = C4
    return std::format("{}{}", noteNameCStr(midiNote), octave);
}

const char* PianoRollEditor::automationTypeName(ClipPreview::AutomationEvent::Type t) noexcept {
    switch (t) {
        case ClipPreview::AutomationEvent::Type::PitchBend:         return "Pitch Bend";
        case ClipPreview::AutomationEvent::Type::PerNotePitchBend:  return "Per-Note Pitch Bend";
        case ClipPreview::AutomationEvent::Type::ChannelPressure:   return "Channel Pressure";
        case ClipPreview::AutomationEvent::Type::PolyPressure:      return "Poly Pressure";
        case ClipPreview::AutomationEvent::Type::ControlChange:     return "Control Change";
        case ClipPreview::AutomationEvent::Type::RPN:               return "RPN (Registered Controller)";
        case ClipPreview::AutomationEvent::Type::NRPN:              return "NRPN (Assignable Controller)";
        case ClipPreview::AutomationEvent::Type::PerNoteParameter:  return "Per-Note Parameter";
    }
    return "Unknown";
}

// ── automation parsing ────────────────────────────────────────────────────────

void PianoRollEditor::parseAutomationFromRaw(
        const ClipPreview::RawMidiData& raw,
        std::vector<EditNote>&                      editNotes,
        std::vector<ClipPreview::AutomationEvent>&  clipEvents) {
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

    auto addPerNoteEvt = [&](uint32_t noteKey, ClipPreview::AutomationEvent evt) {
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
                ClipPreview::AutomationEvent evt{};
                evt.timeSeconds     = t;
                evt.normalizedValue = ump.getMidi1Lsb() / 127.0;
                evt.type            = ClipPreview::AutomationEvent::Type::PolyPressure;
                evt.channel         = channel;
                evt.noteNumber      = note;
                evt.rawEventIdx     = i;
                addPerNoteEvt(baseKey | note, evt);
            } else if (status == umppi::MidiChannelStatus::CC) {
                ClipPreview::AutomationEvent evt{};
                evt.timeSeconds     = t;
                evt.normalizedValue = ump.getMidi1CCData() / 127.0;
                evt.type            = ClipPreview::AutomationEvent::Type::ControlChange;
                evt.channel         = channel;
                evt.paramIndex      = ump.getMidi1CCIndex();
                evt.rawEventIdx     = i;
                clipEvents.push_back(evt);
            } else if (status == umppi::MidiChannelStatus::CAF) {
                ClipPreview::AutomationEvent evt{};
                evt.timeSeconds     = t;
                evt.normalizedValue = ump.getMidi1Msb() / 127.0;
                evt.type            = ClipPreview::AutomationEvent::Type::ChannelPressure;
                evt.channel         = channel;
                evt.rawEventIdx     = i;
                clipEvents.push_back(evt);
            } else if (status == umppi::MidiChannelStatus::PITCH_BEND) {
                ClipPreview::AutomationEvent evt{};
                evt.timeSeconds     = t;
                evt.normalizedValue = ump.getMidi1PitchBendData() / 16383.0;
                evt.type            = ClipPreview::AutomationEvent::Type::PitchBend;
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
                ClipPreview::AutomationEvent evt{};
                evt.timeSeconds     = t;
                evt.normalizedValue = ump.getMidi2PafData() / static_cast<double>(0xFFFFFFFFu);
                evt.type            = ClipPreview::AutomationEvent::Type::PolyPressure;
                evt.channel         = channel;
                evt.noteNumber      = note;
                evt.rawEventIdx     = i;
                addPerNoteEvt(baseKey | note, evt);
            } else if (status == umppi::MidiChannelStatus::PER_NOTE_PITCH_BEND) {
                const uint8_t note = ump.getMidi2Note();
                ClipPreview::AutomationEvent evt{};
                evt.timeSeconds     = t;
                evt.normalizedValue = ump.getMidi2PitchBendData() / static_cast<double>(0xFFFFFFFFu);
                evt.type            = ClipPreview::AutomationEvent::Type::PerNotePitchBend;
                evt.channel         = channel;
                evt.noteNumber      = note;
                evt.rawEventIdx     = i;
                addPerNoteEvt(baseKey | note, evt);
            } else if (status == umppi::MidiChannelStatus::PER_NOTE_RCC ||
                       status == umppi::MidiChannelStatus::PER_NOTE_ACC) {
                const uint8_t note = ump.getMidi2Note();
                ClipPreview::AutomationEvent evt{};
                evt.timeSeconds     = t;
                evt.normalizedValue = ump.getMidi2CcData() / static_cast<double>(0xFFFFFFFFu);
                evt.type            = ClipPreview::AutomationEvent::Type::PerNoteParameter;
                evt.channel         = channel;
                evt.noteNumber      = note;
                evt.paramIndex      = static_cast<uint16_t>(events[i] & 0xFF);
                evt.rawEventIdx     = i;
                addPerNoteEvt(baseKey | note, evt);
            } else if (status == umppi::MidiChannelStatus::CC) {
                ClipPreview::AutomationEvent evt{};
                evt.timeSeconds     = t;
                evt.normalizedValue = ump.getMidi2CcData() / static_cast<double>(0xFFFFFFFFu);
                evt.type            = ClipPreview::AutomationEvent::Type::ControlChange;
                evt.channel         = channel;
                evt.paramIndex      = ump.getMidi2CcIndex();
                evt.rawEventIdx     = i;
                clipEvents.push_back(evt);
            } else if (status == umppi::MidiChannelStatus::RPN) {
                ClipPreview::AutomationEvent evt{};
                evt.timeSeconds     = t;
                evt.normalizedValue = ump.getMidi2RpnData() / static_cast<double>(0xFFFFFFFFu);
                evt.type            = ClipPreview::AutomationEvent::Type::RPN;
                evt.channel         = channel;
                evt.paramIndex      = static_cast<uint16_t>((ump.getMidi2RpnMsb() << 7) | ump.getMidi2RpnLsb());
                evt.rawEventIdx     = i;
                clipEvents.push_back(evt);
            } else if (status == umppi::MidiChannelStatus::NRPN) {
                ClipPreview::AutomationEvent evt{};
                evt.timeSeconds     = t;
                evt.normalizedValue = ump.getMidi2NrpnData() / static_cast<double>(0xFFFFFFFFu);
                evt.type            = ClipPreview::AutomationEvent::Type::NRPN;
                evt.channel         = channel;
                evt.umpGroup        = group;
                evt.paramIndex      = static_cast<uint16_t>((ump.getMidi2NrpnMsb() << 7) | ump.getMidi2NrpnLsb());
                evt.rawEventIdx     = i;
                clipEvents.push_back(evt);
            } else if (status == umppi::MidiChannelStatus::CAF) {
                ClipPreview::AutomationEvent evt{};
                evt.timeSeconds     = t;
                evt.normalizedValue = ump.getMidi2CafData() / static_cast<double>(0xFFFFFFFFu);
                evt.type            = ClipPreview::AutomationEvent::Type::ChannelPressure;
                evt.channel         = channel;
                evt.rawEventIdx     = i;
                clipEvents.push_back(evt);
            } else if (status == umppi::MidiChannelStatus::PITCH_BEND) {
                ClipPreview::AutomationEvent evt{};
                evt.timeSeconds     = t;
                evt.normalizedValue = ump.getMidi2PitchBendData() / static_cast<double>(0xFFFFFFFFu);
                evt.type            = ClipPreview::AutomationEvent::Type::PitchBend;
                evt.channel         = channel;
                evt.rawEventIdx     = i;
                clipEvents.push_back(evt);
            }
        }

        i += static_cast<size_t>(std::max(1, wordCount));
    }
}

// ── public API ───────────────────────────────────────────────────────────────

void PianoRollEditor::showClip(int32_t trackIndex, int32_t clipId,
                                const std::string& clipName,
                                std::shared_ptr<ClipPreview> preview) {
    auto key = std::make_pair(trackIndex, clipId);
    auto& state = windows_[key];
    state.trackIndex = trackIndex;
    state.clipId     = clipId;
    state.clipName   = clipName;
    state.preview    = std::move(preview);
    state.visible    = true;

    // (Re-)initialise editable notes from the freshly-loaded preview.
    // Also reset any in-flight drag to avoid dangling note indices.
    state.editNotes.clear();
    state.editClipEvents.clear();
    if (state.preview) {
        state.editNotes.reserve(state.preview->midiNotes.size());
        for (const auto& n : state.preview->midiNotes)
            state.editNotes.emplace_back(n);
        if (state.preview->rawMidiData)
            parseAutomationFromRaw(*state.preview->rawMidiData, state.editNotes, state.editClipEvents);
    }
    state.drag = DragState{};

    // On first open (vScrollNote still at its default 0), scroll so the
    // pitch range of the clip is centred in the visible area.
    if (state.view.vScrollNote == 0.0f) {
        // Centre the view on the note range of the clip; fall back to C4 (60)
        // when the clip is empty so we don't open at note 127 / C9.
        const float midNote = (!state.editNotes.empty())
                              ? (state.preview->minNote + state.preview->maxNote) * 0.5f
                              : 60.0f;
        float midIdx = static_cast<float>(kNoteCount - 1) - midNote;
        state.view.vScrollNote = std::max(0.0f, midIdx - 8.0f);
    }
}

void PianoRollEditor::render(const RenderContext& ctx) {
    for (auto& [key, state] : windows_)
        if (state.visible)
            renderWindow(state, ctx);

    std::erase_if(windows_, [](const auto& p) { return !p.second.visible; });
}

// ── static helpers ────────────────────────────────────────────────────────────

uint64_t PianoRollEditor::secondsToTicks(double seconds, uint32_t tickRes, double bpm) noexcept {
    if (bpm <= 0.0 || tickRes == 0 || seconds < 0.0) return 0;
    return static_cast<uint64_t>(std::round(seconds * static_cast<double>(tickRes) * bpm / 60.0));
}

void PianoRollEditor::sortRawMidiEvents(std::vector<uapmd_ump_t>& events,
                                         std::vector<uint64_t>&    ticks) {
    // Group consecutive words that form a single UMP message, sort groups by
    // tick, then flatten back to word-per-entry arrays.
    struct Group {
        uint64_t tick{0};
        std::vector<uapmd_ump_t> words;
    };

    std::vector<Group> groups;
    groups.reserve(events.size());

    size_t i = 0;
    while (i < events.size()) {
        umppi::Ump ump(events[i]);
        int wordCount = std::max(1, ump.getSizeInInts());
        Group g;
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

    events.clear();
    ticks.clear();
    for (const auto& g : groups) {
        for (auto w : g.words) {
            events.push_back(w);
            ticks.push_back(g.tick);
        }
    }
}

// ── note write-back ───────────────────────────────────────────────────────────

void PianoRollEditor::applyNoteEdits(WindowState& state, const RenderContext& ctx) {
    state.dirtyAfterEdit = false;
    if (!ctx.applyEdits) return;
    if (!state.preview || !state.preview->rawMidiData) return;

    const ClipPreview::RawMidiData& orig = *state.preview->rawMidiData;
    const uint32_t tickRes = orig.tickResolution;
    const double   bpm     = orig.clipTempo > 0.0 ? orig.clipTempo : 120.0;

    // Default group/channel for new notes — borrowed from the first tracked note.
    uint8_t defaultGroup = 0, defaultChannel = 0;
    if (!state.preview->midiNotes.empty()) {
        const size_t idx0 = state.preview->midiNotes[0].noteOnWordIdx;
        if (idx0 < orig.umpEvents.size()) {
            defaultGroup   = static_cast<uint8_t>((orig.umpEvents[idx0] >> 24) & 0xFu);
            defaultChannel = static_cast<uint8_t>((orig.umpEvents[idx0] >> 16) & 0xFu);
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
    for (const auto& editNote : state.editNotes) {
        markSkip(editNote.noteOnWordIdx);
        markSkip(editNote.noteOffWordIdx);
        for (const auto& ae : editNote.automationEvents)
            markSkip(ae.rawEventIdx);
    }
    for (const auto& ae : state.editClipEvents)
        markSkip(ae.rawEventIdx);
    // Also skip raw events whose in-memory counterpart was deleted this frame.
    for (size_t idx : state.deletedRawIdxs)
        markSkip(idx);
    state.deletedRawIdxs.clear();

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

    // Emit NoteOn + NoteOff for each live (non-deleted) note.
    for (const auto& editNote : state.editNotes) {
        if (editNote.deleted) continue;

        const uint64_t onTick  = secondsToTicks(editNote.startSeconds, tickRes, bpm);
        const uint64_t offTick = secondsToTicks(
            editNote.startSeconds + editNote.durationSeconds, tickRes, bpm);

        // Group / channel: from backing original if available, else defaults.
        const bool   hasBacking = (editNote.noteOnWordIdx < orig.umpEvents.size());
        const uint32_t onWord0  = hasBacking ? orig.umpEvents[editNote.noteOnWordIdx] : 0;
        const uint8_t  grp      = hasBacking
                                  ? static_cast<uint8_t>((onWord0 >> 24) & 0xFu) : defaultGroup;
        const uint8_t  ch       = hasBacking
                                  ? static_cast<uint8_t>((onWord0 >> 16) & 0xFu) : defaultChannel;

        if (editNote.isMidi2 && hasBacking &&
                editNote.noteOnWordIdx + 1 < orig.umpEvents.size()) {
            // MIDI2: preserve attrType / attrData from original words.
            const uint8_t  attrType = static_cast<uint8_t>(onWord0 & 0xFFu);
            const uint16_t attrData = static_cast<uint16_t>(
                orig.umpEvents[editNote.noteOnWordIdx + 1] & 0xFFFFu);
            const uint16_t vel16  = static_cast<uint16_t>(
                std::round(std::clamp(editNote.velocity, 0.0f, 1.0f) * 65535.0f));
            const uint64_t onUmp  = umppi::UmpFactory::midi2NoteOn(
                grp, ch, editNote.note, attrType, vel16, attrData);
            newEvents.push_back(static_cast<uint32_t>(onUmp >> 32));
            newTicks.push_back(onTick);
            newEvents.push_back(static_cast<uint32_t>(onUmp & 0xFFFFFFFFu));
            newTicks.push_back(onTick);

            // NoteOff — reuse original NoteOff attr/vel when present.
            const size_t offIdx = editNote.noteOffWordIdx;
            if (offIdx < orig.umpEvents.size() && offIdx + 1 < orig.umpEvents.size()) {
                const uint32_t offW0     = orig.umpEvents[offIdx];
                const uint8_t  oAttrType = static_cast<uint8_t>(offW0 & 0xFFu);
                const uint16_t oAttrData = static_cast<uint16_t>(
                    orig.umpEvents[offIdx + 1] & 0xFFFFu);
                const uint16_t oVel16    = static_cast<uint16_t>(
                    (orig.umpEvents[offIdx + 1] >> 16) & 0xFFFFu);
                const uint64_t offUmp    = umppi::UmpFactory::midi2NoteOff(
                    grp, ch, editNote.note, oAttrType, oVel16, oAttrData);
                newEvents.push_back(static_cast<uint32_t>(offUmp >> 32));
                newTicks.push_back(offTick);
                newEvents.push_back(static_cast<uint32_t>(offUmp & 0xFFFFFFFFu));
                newTicks.push_back(offTick);
            } else {
                const uint64_t offUmp = umppi::UmpFactory::midi2NoteOff(grp, ch, editNote.note, 0, 0, 0);
                newEvents.push_back(static_cast<uint32_t>(offUmp >> 32));
                newTicks.push_back(offTick);
                newEvents.push_back(static_cast<uint32_t>(offUmp & 0xFFFFFFFFu));
                newTicks.push_back(offTick);
            }
        } else {
            // MIDI1 (also used for brand-new notes, noteOnWordIdx == SIZE_MAX).
            const uint8_t vel7 = static_cast<uint8_t>(
                std::round(std::clamp(editNote.velocity, 0.0f, 1.0f) * 127.0f));
            newEvents.push_back(umppi::UmpFactory::midi1NoteOn(grp, ch, editNote.note, vel7));
            newTicks.push_back(onTick);
            newEvents.push_back(umppi::UmpFactory::midi1NoteOff(grp, ch, editNote.note, 0));
            newTicks.push_back(offTick);
        }

        // Emit per-note automation events (edited or newly added).
        for (const auto& ae : editNote.automationEvents) {
            const uint64_t aeTick = secondsToTicks(ae.timeSeconds, tickRes, bpm);
            const double   v      = std::clamp(ae.normalizedValue, 0.0, 1.0);
            switch (ae.type) {
            case ClipPreview::AutomationEvent::Type::PitchBend: {
                const auto d14 = static_cast<uint16_t>(std::round(v * 16383.0));
                newEvents.push_back(umppi::UmpFactory::midi1PitchBendDirect(grp, ch, d14));
                newTicks.push_back(aeTick);
                break;
            }
            case ClipPreview::AutomationEvent::Type::PerNotePitchBend: {
                const auto d32 = static_cast<uint32_t>(std::round(v * 4294967295.0));
                const uint64_t u2 = umppi::UmpFactory::midi2PerNotePitchBendDirect(
                    grp, ch, editNote.note, d32);
                newEvents.push_back(static_cast<uint32_t>(u2 >> 32));
                newTicks.push_back(aeTick);
                newEvents.push_back(static_cast<uint32_t>(u2 & 0xFFFFFFFFu));
                newTicks.push_back(aeTick);
                break;
            }
            case ClipPreview::AutomationEvent::Type::ChannelPressure: {
                const auto d7 = static_cast<uint8_t>(std::round(v * 127.0));
                newEvents.push_back(umppi::UmpFactory::midi1CAf(grp, ch, d7));
                newTicks.push_back(aeTick);
                break;
            }
            case ClipPreview::AutomationEvent::Type::PolyPressure: {
                const auto d7 = static_cast<uint8_t>(std::round(v * 127.0));
                newEvents.push_back(umppi::UmpFactory::midi1PAf(grp, ch, editNote.note, d7));
                newTicks.push_back(aeTick);
                break;
            }
            case ClipPreview::AutomationEvent::Type::ControlChange: {
                const auto cc  = static_cast<uint8_t>(ae.paramIndex & 0x7Fu);
                const auto d32 = static_cast<uint32_t>(std::round(v * 4294967295.0));
                const uint64_t u2 = umppi::UmpFactory::midi2CC(grp, ch, cc, d32);
                newEvents.push_back(static_cast<uint32_t>(u2 >> 32));
                newTicks.push_back(aeTick);
                newEvents.push_back(static_cast<uint32_t>(u2 & 0xFFFFFFFFu));
                newTicks.push_back(aeTick);
                break;
            }
            case ClipPreview::AutomationEvent::Type::RPN: {
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
            case ClipPreview::AutomationEvent::Type::NRPN: {
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
            case ClipPreview::AutomationEvent::Type::PerNoteParameter: {
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
        }
    }

    // Re-emit edited clip-level automation events.
    // Per-note types can end up here when the user changes the type of a channel-level row:
    // recover the note number by finding whichever editNote contains the event's time.
    auto findNoteNum = [&](const ClipPreview::AutomationEvent& ae) -> uint8_t {
        if (ae.noteNumber != 0) return ae.noteNumber; // still valid (not yet round-tripped)
        for (const auto& en : state.editNotes) {
            if (!en.deleted && ae.timeSeconds >= en.startSeconds &&
                    ae.timeSeconds <= en.startSeconds + en.durationSeconds)
                return en.note;
        }
        return 0; // fallback — note association unknown
    };

    for (const auto& ae : state.editClipEvents) {
        const uint64_t aeTick = secondsToTicks(ae.timeSeconds, tickRes, bpm);
        const double   v      = std::clamp(ae.normalizedValue, 0.0, 1.0);
        // Use defaultGroup; channel comes from the stored AutomationEvent::channel.
        switch (ae.type) {
        case ClipPreview::AutomationEvent::Type::PitchBend: {
            const auto d14 = static_cast<uint16_t>(std::round(v * 16383.0));
            newEvents.push_back(umppi::UmpFactory::midi1PitchBendDirect(defaultGroup, ae.channel, d14));
            newTicks.push_back(aeTick);
            break;
        }
        case ClipPreview::AutomationEvent::Type::ChannelPressure: {
            const auto d7 = static_cast<uint8_t>(std::round(v * 127.0));
            newEvents.push_back(umppi::UmpFactory::midi1CAf(defaultGroup, ae.channel, d7));
            newTicks.push_back(aeTick);
            break;
        }
        case ClipPreview::AutomationEvent::Type::ControlChange: {
            const auto cc  = static_cast<uint8_t>(ae.paramIndex & 0x7Fu);
            const auto d32 = static_cast<uint32_t>(std::round(v * 4294967295.0));
            const uint64_t u2 = umppi::UmpFactory::midi2CC(defaultGroup, ae.channel, cc, d32);
            newEvents.push_back(static_cast<uint32_t>(u2 >> 32));
            newTicks.push_back(aeTick);
            newEvents.push_back(static_cast<uint32_t>(u2 & 0xFFFFFFFFu));
            newTicks.push_back(aeTick);
            break;
        }
        case ClipPreview::AutomationEvent::Type::RPN: {
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
        case ClipPreview::AutomationEvent::Type::NRPN: {
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
        case ClipPreview::AutomationEvent::Type::PolyPressure: {
            const uint8_t noteNum = findNoteNum(ae);
            const auto d7 = static_cast<uint8_t>(std::round(v * 127.0));
            newEvents.push_back(umppi::UmpFactory::midi1PAf(defaultGroup, ae.channel, noteNum, d7));
            newTicks.push_back(aeTick);
            break;
        }
        case ClipPreview::AutomationEvent::Type::PerNotePitchBend: {
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
        case ClipPreview::AutomationEvent::Type::PerNoteParameter: {
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
    }

    sortRawMidiEvents(newEvents, newTicks);

    std::string error;
    if (!ctx.applyEdits(state.trackIndex, state.clipId,
                        std::move(newEvents), std::move(newTicks), error))
        return; // TODO: surface error to the user

    // Reload preview from the freshly-updated engine state.
    if (ctx.reloadPreview) {
        auto newPreview = ctx.reloadPreview(state.trackIndex, state.clipId);
        if (newPreview) {
            state.preview = std::move(newPreview);
            state.editNotes.clear();
            state.editClipEvents.clear();
            state.editNotes.reserve(state.preview->midiNotes.size());
            for (const auto& n : state.preview->midiNotes)
                state.editNotes.emplace_back(n);
            if (state.preview->rawMidiData)
                parseAutomationFromRaw(*state.preview->rawMidiData, state.editNotes, state.editClipEvents);
            state.drag            = DragState{};
            // Preserve the selected note index across reload so the user
            // doesn't lose context when editing automation events.  Only
            // clear it if the reload produced fewer notes than the index.
            if (state.selectedNoteIdx >= static_cast<int>(state.editNotes.size()))
                state.selectedNoteIdx = -1;
            state.noteToDeleteIdx = -1;
        }
    }
}

// ── controls bar ─────────────────────────────────────────────────────────────

void PianoRollEditor::renderControls(WindowState& state, float uiScale) {
    const float itemW = 140.0f * uiScale;

    ImGui::SetNextItemWidth(itemW);
    ImGui::SliderFloat("H Zoom##pr_h", &state.view.hZoom, 10.0f, 2000.0f,
                       "%.0f px/s", ImGuiSliderFlags_Logarithmic);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(itemW);
    ImGui::SliderFloat("V Zoom##pr_v", &state.view.vZoom, 4.0f, 48.0f, "%.0f px");
    ImGui::SameLine();

    // Snap grid selector
    static constexpr const char* kSnapLabels[] = {
        "Free","1/1","1/2","1/4","1/8","1/16","1/32"
    };
    ImGui::SetNextItemWidth(70.0f * uiScale);
    ImGui::Combo("Snap##pr_snap", &state.view.snapIdx, kSnapLabels, 7);

    if (state.preview) {
        ImGui::SameLine();
        ImGui::TextDisabled("  %.2fs  %d notes  %d clip events",
                            state.preview->clipDurationSeconds,
                            static_cast<int>(state.editNotes.size()),
                            static_cast<int>(state.editClipEvents.size()));
    }
}

// ── piano key column ─────────────────────────────────────────────────────────

void PianoRollEditor::renderPianoKeys(ImDrawList* dl, ImVec2 origin, float width, float height,
                                      float noteH, float vScrollPx, float uiScale, int previewNote) const {
    dl->PushClipRect(origin, {origin.x + width, origin.y + height}, true);
    const auto theme = getPianoRollTheme();

    const float rulerH     = kRulerHeight * uiScale;
    const float noteAreaY  = origin.y + rulerH;
    const float noteAreaH  = height - rulerH;

    // Background
    dl->AddRectFilled(origin, {origin.x + width, origin.y + height}, theme.key_panel_bg);
    // Ruler header strip
    dl->AddRectFilled(origin, {origin.x + width, noteAreaY}, theme.key_ruler_bg);

    if (noteAreaH > 0.0f && noteH > 0.0f) {
        int firstIdx = static_cast<int>(vScrollPx / noteH);
        int lastIdx  = static_cast<int>((vScrollPx + noteAreaH) / noteH) + 1;
        firstIdx = std::max(0, firstIdx);
        lastIdx  = std::min(kNoteCount - 1, lastIdx);

        const float blackKeyW = width * 0.62f;

        for (int idx = firstIdx; idx <= lastIdx; ++idx) {
            int midiNote = (kNoteCount - 1) - idx;
            float y0 = noteAreaY + idx * noteH - vScrollPx;
            float y1 = y0 + noteH;

            const bool isPreview = (midiNote == previewNote);
            if (isBlackKey(midiNote)) {
                ImU32 keyCol = isPreview ? theme.key_preview_black : theme.key_black;
                dl->AddRectFilled({origin.x, y0}, {origin.x + blackKeyW, y1 - 0.5f}, keyCol);
            } else {
                ImU32 keyCol = isPreview ? theme.key_preview_white : theme.key_white;
                dl->AddRectFilled({origin.x, y0}, {origin.x + width, y1 - 0.5f}, keyCol);
                // Octave label on every C note
                if (midiNote % 12 == 0) {
                    auto label  = fullNoteName(midiNote);
                    float fontH = ImGui::GetFontSize();
                    float textY = y0 + (noteH - fontH) * 0.5f;
                    if (textY >= noteAreaY && textY + fontH <= noteAreaY + noteAreaH)
                        dl->AddText({origin.x + 2.0f, textY}, theme.key_label, label.c_str());
                }
            }
            // Row separator
            dl->AddLine({origin.x, y1 - 0.5f}, {origin.x + width, y1 - 0.5f},
                         theme.key_separator);
        }
    }

    // Right border
    dl->AddLine({origin.x + width - 1.0f, origin.y + rulerH},
                 {origin.x + width - 1.0f, origin.y + height},
                 theme.key_border);

    dl->PopClipRect();
}

// ── note grid ────────────────────────────────────────────────────────────────

void PianoRollEditor::renderNoteGrid(ImDrawList* dl, ImVec2 origin, float width, float height,
                                     float noteH, float pxPerSec, float hScroll, float vScrollPx,
                                     WindowState& state, float uiScale) const {
    if (width <= 0.0f || height <= 0.0f) return;
    const auto theme = getPianoRollTheme();

    const float rulerH    = kRulerHeight * uiScale;
    const float noteAreaY = origin.y + rulerH;
    const float noteAreaH = height - rulerH;

    dl->PushClipRect(origin, {origin.x + width, origin.y + height}, true);

    // ── Background ───────────────────────────────────────────────────────────
    dl->AddRectFilled(origin, {origin.x + width, origin.y + height}, theme.editor_bg);

    // ── Note lane bands ──────────────────────────────────────────────────────
    if (noteAreaH > 0.0f && noteH > 0.0f) {
        int firstIdx = static_cast<int>(vScrollPx / noteH);
        int lastIdx  = static_cast<int>((vScrollPx + noteAreaH) / noteH) + 1;
        firstIdx = std::max(0, firstIdx);
        lastIdx  = std::min(kNoteCount - 1, lastIdx);

        for (int idx = firstIdx; idx <= lastIdx; ++idx) {
            int midiNote = (kNoteCount - 1) - idx;
            float y0 = noteAreaY + idx * noteH - vScrollPx;
            float y1 = y0 + noteH;

            ImU32 laneCol = isBlackKey(midiNote) ? theme.lane_black : theme.lane_white;
            if (midiNote % 12 == 0) laneCol = theme.lane_octave;
            dl->AddRectFilled({origin.x, y0}, {origin.x + width, y1}, laneCol);
            dl->AddLine({origin.x, y1 - 0.5f}, {origin.x + width, y1 - 0.5f},
                         theme.lane_separator);
        }
    }

    // ── BPM (shared by beat lines, ruler, and snap) ──────────────────────────
    double bpm = 120.0;
    if (state.preview && !state.preview->tempoPoints.empty() &&
            state.preview->tempoPoints[0].bpm > 0.0)
        bpm = state.preview->tempoPoints[0].bpm;

    // ── Vertical beat / bar lines ─────────────────────────────────────────────
    if (pxPerSec > 0.0f && state.preview) {
        const float beatPx = static_cast<float>(60.0 / bpm) * pxPerSec;
        const float barPx  = beatPx * 4.0f;

        if (barPx >= 2.0f) {
            float firstBarX = origin.x - std::fmod(hScroll, barPx);
            for (float x = firstBarX; x <= origin.x + width; x += barPx)
                dl->AddLine({x, noteAreaY}, {x, origin.y + height},
                             theme.bar_line, 1.5f);

            if (beatPx >= 6.0f) {
                float firstBeatX = origin.x - std::fmod(hScroll, beatPx);
                for (float x = firstBeatX; x <= origin.x + width; x += beatPx)
                    dl->AddLine({x, noteAreaY}, {x, origin.y + height},
                                 theme.beat_line);
            }
        }
    }

    // ── Drag update (runs every frame before notes are drawn) ─────────────────
    // Snap values match the kSnapLabels[] order in renderControls.
    static constexpr float  kSnapValues[7]   = { 0.f, 1.f, 0.5f, 0.25f, 0.125f, 0.0625f, 0.03125f };
    static constexpr double kMinNoteDuration = 0.01; // seconds
    if (state.drag.active) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const float  dx        = ImGui::GetIO().MousePos.x - state.drag.startMouseX;
            const float  dy        = ImGui::GetIO().MousePos.y - state.drag.startMouseY;
            const float  snapBeats = kSnapValues[std::clamp(state.view.snapIdx, 0, 6)];
            const double snapSec   = (snapBeats > 0.0f && bpm > 0.0)
                                     ? static_cast<double>(snapBeats) * 60.0 / bpm : 0.0;

            if (state.drag.noteIdx >= 0 &&
                    state.drag.noteIdx < static_cast<int>(state.editNotes.size())) {
                auto& n = state.editNotes[state.drag.noteIdx];

                if (state.drag.mode == DragState::Mode::Move) {
                    double newStart = state.drag.origStartSec + static_cast<double>(dx) / pxPerSec;
                    int    newNote  = state.drag.origNoteNum -
                                     static_cast<int>(std::round(static_cast<double>(dy) / noteH));
                    if (snapSec > 0.0) newStart = std::round(newStart / snapSec) * snapSec;
                    n.startSeconds = std::max(0.0, newStart);
                    n.note         = static_cast<uint8_t>(std::clamp(newNote, 0, 127));
                } else if (state.drag.mode == DragState::Mode::ResizeRight) {
                    double newEnd = state.drag.origEndSec + static_cast<double>(dx) / pxPerSec;
                    if (snapSec > 0.0) newEnd = std::round(newEnd / snapSec) * snapSec;
                    n.startSeconds    = state.drag.origStartSec;
                    n.durationSeconds = std::max(kMinNoteDuration, newEnd - state.drag.origStartSec);
                    n.note            = static_cast<uint8_t>(state.drag.origNoteNum);
                } else { // ResizeLeft
                    double newStart = state.drag.origStartSec + static_cast<double>(dx) / pxPerSec;
                    if (snapSec > 0.0) newStart = std::round(newStart / snapSec) * snapSec;
                    newStart       = std::clamp(newStart, 0.0, state.drag.origEndSec - kMinNoteDuration);
                    n.startSeconds    = newStart;
                    n.durationSeconds = state.drag.origEndSec - newStart;
                    n.note            = static_cast<uint8_t>(state.drag.origNoteNum);
                }
            }
        } else {
            // Mouse released — finalise drag; schedule write-back if anything changed.
            if (state.drag.noteIdx >= 0 &&
                    state.drag.noteIdx < static_cast<int>(state.editNotes.size())) {
                const auto& n = state.editNotes[state.drag.noteIdx];
                if (n.startSeconds != state.drag.origStartSec ||
                        n.startSeconds + n.durationSeconds != state.drag.origEndSec ||
                        static_cast<int>(n.note) != state.drag.origNoteNum)
                    state.dirtyAfterEdit = true;
            }
            state.drag.active  = false;
            state.drag.noteIdx = -1;
        }
    }

    // ── Note rectangles ───────────────────────────────────────────────────────
    // Edge threshold (px) within which dragging resizes instead of moves.
    static constexpr float kResizeEdgePx = 8.0f;
    if (!state.editNotes.empty() && noteAreaH > 0.0f) {
        // Capture click intent before the loop; consumed by the first hit note.
        bool   mouseClick = !state.drag.active &&
                            ImGui::IsWindowHovered() &&
                            ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        ImVec2 mousePos   = ImGui::GetMousePos();
        bool   hoverNote  = false;
        bool   hoverEdge  = false;

        for (int ni = 0; ni < static_cast<int>(state.editNotes.size()); ++ni) {
            const auto& note = state.editNotes[ni];
            if (note.deleted) continue;

            int noteIdx = (kNoteCount - 1) - static_cast<int>(note.note);

            float y0 = noteAreaY + noteIdx * noteH - vScrollPx;
            float y1 = y0 + noteH - 1.0f;
            float x0 = origin.x + static_cast<float>(note.startSeconds) * pxPerSec - hScroll;
            float x1 = x0 + std::max(2.0f * uiScale,
                                      static_cast<float>(note.durationSeconds) * pxPerSec);

            if (x1 < origin.x || x0 > origin.x + width) continue;
            if (y1 < noteAreaY || y0 > noteAreaY + noteAreaH) continue;

            const bool  selected = (ni == state.selectedNoteIdx);
            const bool  dragging = (state.drag.active && ni == state.drag.noteIdx);
            const float vel      = note.velocity; // already 0-1

            ImU32 fillCol = (selected || dragging)
                ? theme.note_selected_fill
                : ImGui::GetColorU32(mixColor(
                    ImGui::ColorConvertU32ToFloat4(theme.note_fill_low),
                    ImGui::ColorConvertU32ToFloat4(theme.note_fill_high),
                    std::clamp(vel, 0.0f, 1.0f)));

            float r = std::min(2.0f * uiScale, noteH * 0.35f);
            dl->AddRectFilled({x0, y0}, {x1, y1}, fillCol, r);
            dl->AddRect({x0, y0}, {x1, y1},
                         (selected || dragging) ? theme.note_selected_border
                                                : theme.note_border,
                         r, 0, 1.0f);

            // Dot indicator on notes with per-note automation events
            if (!note.automationEvents.empty()) {
                float dotR = std::min(3.0f * uiScale, noteH * 0.25f);
                float dotX = std::min(x1 - dotR - 1.0f * uiScale, x0 + (x1 - x0) * 0.8f);
                float dotY = y0 + (y1 - y0) * 0.5f;
                dl->AddCircleFilled({dotX, dotY}, dotR, theme.automation_dot);
            }

            // Resize-edge hit zones (30% of width, max kResizeEdgePx*uiScale).
            const float noteW   = x1 - x0;
            const float edgePx  = std::min(kResizeEdgePx * uiScale, noteW * 0.3f);

            // Hit-test (full note rect)
            const bool overNote    = (mousePos.x >= x0 && mousePos.x <= x1 &&
                                      mousePos.y >= y0 && mousePos.y <= y1);
            const bool atLeftEdge  = overNote && (mousePos.x < x0 + edgePx);
            const bool atRightEdge = overNote && !atLeftEdge && (mousePos.x > x1 - edgePx);

            if (overNote) {
                hoverNote = true;
                if (atLeftEdge || atRightEdge) hoverEdge = true;
            }

            // Drag / select on click
            if (mouseClick && overNote) {
                state.drag.active       = true;
                state.drag.noteIdx      = ni;
                state.drag.startMouseX  = ImGui::GetIO().MousePos.x;
                state.drag.startMouseY  = ImGui::GetIO().MousePos.y;
                state.drag.origStartSec = note.startSeconds;
                state.drag.origEndSec   = note.startSeconds + note.durationSeconds;
                state.drag.origNoteNum  = static_cast<int>(note.note);
                if (atRightEdge)      state.drag.mode = DragState::Mode::ResizeRight;
                else if (atLeftEdge)  state.drag.mode = DragState::Mode::ResizeLeft;
                else                  state.drag.mode = DragState::Mode::Move;
                state.selectedNoteIdx = ni;
                mouseClick = false; // consume so only the top-most note is picked
            }
        }

        // Click on empty space → deselect
        if (mouseClick)
            state.selectedNoteIdx = -1;

        // Cursor: EW-resize for edges, ResizeAll for body; same when drag is active.
        if (state.drag.active) {
            if (state.drag.mode != DragState::Mode::Move)
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            else
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        } else if (hoverEdge) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        } else if (hoverNote) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        }
    }

    // ── Double-click: delete existing note or insert a new 1/4-note ──────────
    if (noteAreaH > 0.0f && noteH > 0.0f &&
            ImGui::IsWindowHovered() &&
            ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
            state.preview && state.preview->rawMidiData) {
        const ImVec2 mp = ImGui::GetMousePos();
        if (mp.y > noteAreaY && mp.y < noteAreaY + noteAreaH) {
            // Find any existing, non-deleted note under the cursor.
            int hitIdx = -1;
            for (int ni = 0; ni < static_cast<int>(state.editNotes.size()); ++ni) {
                const auto& n = state.editNotes[ni];
                if (n.deleted) continue;
                int   nIdx = (kNoteCount - 1) - static_cast<int>(n.note);
                float y0   = noteAreaY + nIdx * noteH - vScrollPx;
                float y1   = y0 + noteH - 1.0f;
                float x0   = origin.x + static_cast<float>(n.startSeconds) * pxPerSec - hScroll;
                float x1   = x0 + std::max(2.0f * uiScale,
                                           static_cast<float>(n.durationSeconds) * pxPerSec);
                if (mp.x >= x0 && mp.x <= x1 && mp.y >= y0 && mp.y <= y1) {
                    hitIdx = ni;
                    break;
                }
            }

            if (hitIdx >= 0) {
                // Double-click on existing note → request deletion with confirmation.
                state.noteToDeleteIdx  = hitIdx;
                state.needsDeletePopup = true;
            } else {
                // Double-click on empty space → insert a new 1/4-note.
                double time = static_cast<double>(mp.x - origin.x + hScroll) / pxPerSec;
                int slot     = static_cast<int>((mp.y - noteAreaY + vScrollPx) / noteH);
                int midiNote = (kNoteCount - 1) - std::clamp(slot, 0, kNoteCount - 1);

                // Quarter-note duration
                const double noteDuration = (bpm > 0.0) ? 60.0 / bpm : 0.5;

                // Snap to active grid
                const float snapBeats = kSnapValues[std::clamp(state.view.snapIdx, 0, 6)];
                if (snapBeats > 0.0f && bpm > 0.0) {
                    const double snapSec = static_cast<double>(snapBeats) * 60.0 / bpm;
                    time = std::round(time / snapSec) * snapSec;
                }
                time = std::max(0.0, time);

                EditNote newNote;
                newNote.startSeconds    = time;
                newNote.durationSeconds = noteDuration;
                newNote.note            = static_cast<uint8_t>(std::clamp(midiNote, 0, 127));
                newNote.velocity        = 0.787f; // ≈ 100/127
                newNote.channel         = 0;
                newNote.isMidi2         = false;
                newNote.noteOnWordIdx   = SIZE_MAX; // marks as new — no backing raw event
                newNote.noteOffWordIdx  = SIZE_MAX;
                state.editNotes.push_back(std::move(newNote));
                state.selectedNoteIdx = static_cast<int>(state.editNotes.size()) - 1;
                state.dirtyAfterEdit  = true;
            }
        }
    }

    // ── Time ruler (top strip — H-scrolled, not V-scrolled) ──────────────────
    dl->AddRectFilled(origin, {origin.x + width, origin.y + rulerH}, theme.ruler_bg);

    if (pxPerSec > 0.0f && state.preview) {
        // bpm already computed above
        const float barPx  = static_cast<float>(60.0 / bpm) * pxPerSec * 4.0f;
        if (barPx >= 2.0f) {
            double firstBarSecs = std::floor(hScroll / barPx) * (60.0 / bpm) * 4.0;
            int barNum = static_cast<int>(firstBarSecs / ((60.0 / bpm) * 4.0));
            float x = origin.x + static_cast<float>(firstBarSecs) * pxPerSec - hScroll;

            while (x <= origin.x + width) {
                dl->AddLine({x, origin.y}, {x, origin.y + rulerH},
                             theme.ruler_line);
                std::string label = std::to_string(barNum + 1);
                float textY = origin.y + (rulerH - ImGui::GetFontSize()) * 0.5f;
                if (x + 2.0f < origin.x + width - 4.0f)
                    dl->AddText({x + 2.0f, textY}, theme.ruler_text, label.c_str());
                x += barPx;
                ++barNum;
            }
        }
    }
    dl->AddLine({origin.x, origin.y + rulerH}, {origin.x + width, origin.y + rulerH},
                 theme.ruler_bottom_line);

    dl->PopClipRect();
}

// ── NRPN parameter picker popup ───────────────────────────────────────────────

bool PianoRollEditor::renderNrpnPicker(
    const char* popupId,
    uint16_t& paramIndex,
    uint8_t&  umpGroup,
    int& hoveredPlugin,
    const std::vector<PluginParamEntry>& entries)
{
    if (entries.empty())
        return false;

    bool changed = false;

    ImGui::SetNextWindowSize(ImVec2(560.0f, 320.0f), ImGuiCond_Always);
    if (!ImGui::BeginPopup(popupId))
        return false;

    // Clamp hoveredPlugin to valid range (entries may have changed since last open).
    if (hoveredPlugin < 0 || hoveredPlugin >= static_cast<int>(entries.size()))
        hoveredPlugin = 0;

    const float leftW = 160.0f;

    // ── Left pane: plugin list (labelled "[group] PluginName") ───────────────
    ImGui::BeginChild("##nrpn_plugins", ImVec2(leftW, 0.0f), true,
                       ImGuiWindowFlags_None);
    for (int pi = 0; pi < static_cast<int>(entries.size()); ++pi) {
        char label[256];
        snprintf(label, sizeof(label), "[%u] %s",
                 static_cast<unsigned>(entries[pi].group),
                 entries[pi].pluginName.c_str());
        const bool sel = (pi == hoveredPlugin);
        if (UapmdSelectable(label, sel))
            hoveredPlugin = pi;
        if (ImGui::IsItemHovered())
            hoveredPlugin = pi;
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // ── Right pane: parameter table ──────────────────────────────────────────
    ImGui::BeginChild("##nrpn_params", ImVec2(0.0f, 0.0f), true,
                       ImGuiWindowFlags_None);
    const auto& entry = entries[hoveredPlugin];

    if (ImGui::BeginTable("##nrpn_pt", 3,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit,
            ImGui::GetContentRegionAvail()))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Index", ImGuiTableColumnFlags_WidthFixed, 45.0f);
        ImGui::TableSetupColumn("Path",  ImGuiTableColumnFlags_WidthStretch, 0.4f);
        ImGui::TableSetupColumn("Name",  ImGuiTableColumnFlags_WidthStretch, 0.6f);
        ImGui::TableHeadersRow();

        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(entry.params.size()));
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const auto& p = entry.params[row];
                ImGui::PushID(row);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                char idxBuf[16];
                snprintf(idxBuf, sizeof(idxBuf), "%u", static_cast<unsigned>(p.nrpnIndex));
                const bool rowSel = (p.nrpnIndex == paramIndex && entry.group == umpGroup);
                if (UapmdSelectable(idxBuf, rowSel, ImGuiSelectableFlags_SpanAllColumns)) {
                    paramIndex = p.nrpnIndex;
                    umpGroup   = entry.group;
                    changed = true;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(p.path.c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(p.name.c_str());
                ImGui::PopID();
            }
        }
        clipper.End();
        ImGui::EndTable();
    }
    ImGui::EndChild();

    ImGui::EndPopup();
    return changed;
}

// ── automation event list ─────────────────────────────────────────────────────

void PianoRollEditor::renderAutomationPanel(WindowState& state, const RenderContext& ctx) const {
    const float uiScale = ctx.uiScale;
    const auto theme = getPianoRollTheme();
    if (!state.preview) {
        ImGui::TextDisabled("No clip loaded.");
        return;
    }

    const bool hasSelection = state.selectedNoteIdx >= 0 &&
        state.selectedNoteIdx < static_cast<int>(state.editNotes.size());

    if (!hasSelection) {
        // Show clip-level automation events
        ImGui::TextDisabled("No note selected — showing %d clip-level automation events.",
                            static_cast<int>(state.editClipEvents.size()));

        if (state.editClipEvents.empty()) return;

        if (ImGui::BeginTable("##clipAuto", 4,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit |
                ImGuiTableFlags_Resizable,
                ImVec2(0, 160.0f * uiScale))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Time (s)",  ImGuiTableColumnFlags_WidthFixed,   60.0f * uiScale);
            ImGui::TableSetupColumn("Type",      ImGuiTableColumnFlags_WidthFixed,  100.0f * uiScale);
            ImGui::TableSetupColumn("Param (MSB\xc2\xb7LSB)", ImGuiTableColumnFlags_WidthFixed,  120.0f * uiScale);
            ImGui::TableSetupColumn("Value",     ImGuiTableColumnFlags_WidthFixed,  300.0f * uiScale);
            ImGui::TableHeadersRow();

            const int evtCount = static_cast<int>(state.editClipEvents.size());
            for (int ci = 0; ci < evtCount; ++ci) {
                const auto& evt = state.editClipEvents[ci];
                ImGui::PushID(ci);
                ImGui::TableNextRow();

                // Col 0: time — also acts as the click target for the whole row.
                ImGui::TableSetColumnIndex(0);
                char timeLabel[32];
                std::snprintf(timeLabel, sizeof(timeLabel), "%.4f", evt.timeSeconds);
                const bool rowClicked = ImGui::Selectable(timeLabel, false,
                    ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
                    ImVec2(0, ImGui::GetTextLineHeight()));
                if (rowClicked) {
                    // Find the note whose time-range contains this event and select it.
                    for (int ni = 0; ni < static_cast<int>(state.editNotes.size()); ++ni) {
                        const auto& n = state.editNotes[ni];
                        if (!n.deleted &&
                                evt.timeSeconds >= n.startSeconds &&
                                evt.timeSeconds <= n.startSeconds + n.durationSeconds) {
                            state.selectedNoteIdx = ni;
                            // Scroll the piano-roll vertically to show the note.
                            const float targetIdx = static_cast<float>(kNoteCount - 1) -
                                                    static_cast<float>(n.note);
                            state.view.vScrollNote = std::max(0.0f, targetIdx - 8.0f);
                            break;
                        }
                    }
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Click to jump to the note containing this event");

                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(automationTypeName(evt.type));
                ImGui::TableSetColumnIndex(2);
                if (evt.paramIndex > 0)
                    ImGui::Text("%u", static_cast<unsigned>(evt.paramIndex));
                else
                    ImGui::TextDisabled("—");
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%.4f", evt.normalizedValue);

                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        return;
    }

    auto& note = state.editNotes[state.selectedNoteIdx];
    ImGui::Text("Note: %s (MIDI %u)  Ch: %u  %.4fs  dur: %.4fs  — %d per-note events",
                fullNoteName(note.note).c_str(),
                static_cast<unsigned>(note.note),
                static_cast<unsigned>(note.channel),
                note.startSeconds,
                note.durationSeconds,
                static_cast<int>(note.automationEvents.size()));
    ImGui::SameLine();
    float vel = note.velocity;
    ImGui::SetNextItemWidth(200.0f);
    ImGui::SliderFloat("Velocity##pr_vel", &vel, 0.0f, 1.0f, "%.3f");
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        note.velocity = std::clamp(vel, 0.0f, 1.0f);
        state.dirtyAfterEdit = true;
    } else if (ImGui::IsItemActive()) {
        note.velocity = std::clamp(vel, 0.0f, 1.0f); // live preview in panel
    }

    // Type names in enum order (PitchBend=0 … PerNoteParameter=7)
    static const char* const kAutoTypeNames[] = {
        "Pitch Bend", "Per-Note Pitch Bend", "Channel Pressure", "Poly Pressure",
        "Control Change", "RPN (Registered Controller)", "NRPN (Assignable Controller)",
        "Per-Note Parameter"
    };
    static constexpr int kAutoTypeCount = 8;
    // MIDI "switch" CCs — value is logically on/off (sustain, portamento, …)
    auto isSwitchCC = [](uint16_t p) { return p >= 64 && p <= 69; };

    int pendingDelete      = -1;
    int pendingDeleteClip  = -1;
    int pendingInsertNote  = -1;
    int pendingInsertClip  = -1;

    // Context-menu column: same square width as before.
    // Column must be wider by 2*CellPadding.x so the inner content area equals the button width.
    const float delBtnW = ImGui::GetFrameHeight();
    const float delColW = delBtnW + 2.0f * ImGui::GetStyle().CellPadding.x;

    if (ImGui::BeginTable("##noteAuto", 5,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit |
            ImGuiTableFlags_Resizable,
            ImVec2(0, 160.0f * uiScale))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        // NoHeaderLabel: hide "Del" text so the column stays at the minimal square width.
        // NoResize: prevent the user from dragging this utility column wider.
        ImGui::TableSetupColumn("##del",
            ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize, delColW);
        ImGui::TableSetupColumn("Time (s)",  ImGuiTableColumnFlags_WidthFixed,   60.0f * uiScale);
        ImGui::TableSetupColumn("Type",      ImGuiTableColumnFlags_WidthFixed,  100.0f * uiScale);
        ImGui::TableSetupColumn("Param (MSB\xc2\xb7LSB)", ImGuiTableColumnFlags_WidthFixed,  120.0f * uiScale);
        ImGui::TableSetupColumn("Value",     ImGuiTableColumnFlags_WidthFixed,  300.0f * uiScale);
        ImGui::TableHeadersRow();

        // Per-note events (editable)
        for (int ei = 0; ei < static_cast<int>(note.automationEvents.size()); ++ei) {
            auto& ae = note.automationEvents[ei];
            ImGui::PushID(ei);
            ImGui::TableNextRow();

            // Context-menu button (col 0 — always visible, never clipped)
            ImGui::TableSetColumnIndex(0);
            {
                const std::string menuId = std::format("RowActionsN##{}", ei);
                if (contextActionButton(std::format("{}##N{}", icons::ContextMenu, ei).c_str(),
                        ImVec2(ImGui::GetContentRegionAvail().x, ImGui::GetFrameHeight())))
                    ImGui::OpenPopup(menuId.c_str());
                if (ImGui::BeginPopup(menuId.c_str())) {
                    if (contextActionMenuItem("Insert Event Before")) {
                        pendingInsertNote = ei;
                        ImGui::CloseCurrentPopup();
                    }
                    if (contextActionMenuItem("Delete Event")) {
                        pendingDelete = ei;
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::EndPopup();
                }
            }

            // Time
            ImGui::TableSetColumnIndex(1);
            double t = ae.timeSeconds;
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::InputDouble("##t", &t, 0.0, 0.0, "%.4f"))
                ae.timeSeconds = std::clamp(t, note.startSeconds,
                                            note.startSeconds + note.durationSeconds);
            if (ImGui::IsItemDeactivatedAfterEdit())
                state.dirtyAfterEdit = true;

            // Type
            ImGui::TableSetColumnIndex(2);
            int typeIdx = static_cast<int>(ae.type);
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::Combo("##type", &typeIdx, kAutoTypeNames, kAutoTypeCount)) {
                ae.type = static_cast<ClipPreview::AutomationEvent::Type>(typeIdx);
                state.dirtyAfterEdit = true;
            }

            // Param
            ImGui::TableSetColumnIndex(3);
            using T = ClipPreview::AutomationEvent::Type;
            if (ae.type == T::ControlChange || ae.type == T::PerNoteParameter) {
                int p = static_cast<int>(ae.paramIndex);
                const int pMax = (ae.type == T::ControlChange) ? 127 : 255;
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::InputInt("##param", &p, 0, 0))
                    ae.paramIndex = static_cast<uint16_t>(std::clamp(p, 0, pMax));
                if (ImGui::IsItemDeactivatedAfterEdit())
                    state.dirtyAfterEdit = true;
            } else if (ae.type == T::RPN || ae.type == T::NRPN) {
                // For NRPN, fetch plugin entries for bound-name display and picker.
                std::vector<PluginParamEntry> nrpnEntries;
                const char* boundName = nullptr;
                if (ae.type == T::NRPN && ctx.getTrackPluginParameters) {
                    nrpnEntries = ctx.getTrackPluginParameters(state.trackIndex);
                    for (const auto& entry : nrpnEntries)
                        if (entry.group == ae.umpGroup)
                            for (const auto& p : entry.params)
                                if (p.nrpnIndex == ae.paramIndex) { boundName = p.name.c_str(); break; }
                }

                // Slider label: bound param name (% escaped, with group prefix when
                // multiple plugin entries exist to help disambiguation) or "MSB:x / LSB:y"
                char sliderLabel[256] = {};
                if (boundName) {
                    char* dst = sliderLabel;
                    // Prefix "[G:N] " when ≥2 plugins share the track
                    if (nrpnEntries.size() >= 2) {
                        int prefixLen = snprintf(dst, sizeof(sliderLabel), "[G:%u] ", ae.umpGroup);
                        if (prefixLen > 0) dst += prefixLen;
                    }
                    const char* src = boundName;
                    while (*src && dst < sliderLabel + sizeof(sliderLabel) - 2) {
                        if (*src == '%') *dst++ = '%';
                        *dst++ = *src++;
                    }
                } else {
                    snprintf(sliderLabel, sizeof(sliderLabel), "MSB:%d / LSB:%d",
                             ae.paramIndex >> 7, ae.paramIndex & 0x7F);
                }

                const bool hasPickerBtn = !nrpnEntries.empty();
                const float frameH  = ImGui::GetFrameHeight();
                const float spacing = ImGui::GetStyle().ItemSpacing.x;
                const float nBtns   = hasPickerBtn ? 3.0f : 2.0f;
                const float sliderW = std::max(
                    ImGui::GetContentRegionAvail().x - nBtns * (frameH + spacing),
                    20.0f * uiScale);

                // [−] decrement
                if (ImGui::Button("-##dec", ImVec2(frameH, frameH)))
                    if (ae.paramIndex > 0) { --ae.paramIndex; state.dirtyAfterEdit = true; }
                ImGui::SameLine();

                // Slider (0 – 16383 combined NRPN index)
                int combined = static_cast<int>(ae.paramIndex);
                ImGui::SetNextItemWidth(sliderW);
                if (ImGui::SliderInt("##nrpn", &combined, 0, 16383, sliderLabel)) {
                    ae.paramIndex = static_cast<uint16_t>(combined);
                    state.dirtyAfterEdit = true;
                }
                ImGui::SameLine();

                // [+] increment
                if (ImGui::Button("+##inc", ImVec2(frameH, frameH)))
                    if (ae.paramIndex < 16383) { ++ae.paramIndex; state.dirtyAfterEdit = true; }

                // [▼] picker arrow (NRPN only, when entries are available)
                if (hasPickerBtn) {
                    ImGui::SameLine();
                    if (contextActionArrowButton("##nrpnpick", ImGuiDir_Down))
                        ImGui::OpenPopup("##nrpnpicker");
                    if (renderNrpnPicker("##nrpnpicker", ae.paramIndex, ae.umpGroup,
                            state.nrpnPickerHoveredPlugin, nrpnEntries))
                        state.dirtyAfterEdit = true;
                }
            } else {
                ImGui::TextDisabled("—");
            }

            // Value
            ImGui::TableSetColumnIndex(4);
            const bool isCC = (ae.type == T::ControlChange);
            if (isCC && isSwitchCC(ae.paramIndex)) {
                // Boolean switch CC: show Off / On combo
                int onOff = ae.normalizedValue >= 0.5 ? 1 : 0;
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::Combo("##val", &onOff, "Off\0On\0")) {
                    ae.normalizedValue = onOff ? 1.0 : 0.0;
                    state.dirtyAfterEdit = true;
                }
            } else {
                float v = static_cast<float>(ae.normalizedValue);
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::SliderFloat("##val", &v, 0.0f, 1.0f, "%.4f"))
                    ae.normalizedValue = static_cast<double>(v);
                if (ImGui::IsItemDeactivatedAfterEdit())
                    state.dirtyAfterEdit = true;
            }

            ImGui::PopID();
        }

        // Channel-level events that overlap with this note's active time window (editable)
        for (int ci = 0; ci < static_cast<int>(state.editClipEvents.size()); ++ci) {
            auto& ae = state.editClipEvents[ci];
            if (ae.timeSeconds < note.startSeconds) continue;
            if (ae.timeSeconds > note.startSeconds + note.durationSeconds) continue;

            ImGui::PushID(1000 + ci); // offset avoids ID collision with per-note rows
            ImGui::TableNextRow();
            ImGui::PushStyleColor(ImGuiCol_Text, theme.automation_related_text);

            // Context-menu button (col 0 — always visible, never clipped)
            ImGui::TableSetColumnIndex(0);
            {
                const std::string menuId = std::format("RowActionsC##{}", ci);
                if (contextActionButton(std::format("{}##C{}", icons::ContextMenu, ci).c_str(),
                        ImVec2(ImGui::GetContentRegionAvail().x, ImGui::GetFrameHeight())))
                    ImGui::OpenPopup(menuId.c_str());
                if (ImGui::BeginPopup(menuId.c_str())) {
                    if (contextActionMenuItem("Insert Event Before")) {
                        pendingInsertClip = ci;
                        ImGui::CloseCurrentPopup();
                    }
                    if (contextActionMenuItem("Delete Event")) {
                        pendingDeleteClip = ci;
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::EndPopup();
                }
            }

            // Time
            ImGui::TableSetColumnIndex(1);
            double t = ae.timeSeconds;
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::InputDouble("##t", &t, 0.0, 0.0, "%.4f"))
                ae.timeSeconds = std::max(0.0, t);
            if (ImGui::IsItemDeactivatedAfterEdit())
                state.dirtyAfterEdit = true;

            // Type
            ImGui::TableSetColumnIndex(2);
            int typeIdx = static_cast<int>(ae.type);
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::Combo("##type", &typeIdx, kAutoTypeNames, kAutoTypeCount)) {
                ae.type = static_cast<ClipPreview::AutomationEvent::Type>(typeIdx);
                state.dirtyAfterEdit = true;
            }

            // Param
            ImGui::TableSetColumnIndex(3);
            using T = ClipPreview::AutomationEvent::Type;
            if (ae.type == T::ControlChange || ae.type == T::PerNoteParameter) {
                int p = static_cast<int>(ae.paramIndex);
                const int pMax = (ae.type == T::ControlChange) ? 127 : 255;
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::InputInt("##param", &p, 0, 0))
                    ae.paramIndex = static_cast<uint16_t>(std::clamp(p, 0, pMax));
                if (ImGui::IsItemDeactivatedAfterEdit())
                    state.dirtyAfterEdit = true;
            } else if (ae.type == T::RPN || ae.type == T::NRPN) {
                // For NRPN, fetch plugin entries for bound-name display and picker.
                std::vector<PluginParamEntry> nrpnEntries;
                const char* boundName = nullptr;
                if (ae.type == T::NRPN && ctx.getTrackPluginParameters) {
                    nrpnEntries = ctx.getTrackPluginParameters(state.trackIndex);
                    for (const auto& entry : nrpnEntries)
                        if (entry.group == ae.umpGroup)
                            for (const auto& p : entry.params)
                                if (p.nrpnIndex == ae.paramIndex) { boundName = p.name.c_str(); break; }
                }

                // Slider label: bound param name (% escaped, with group prefix when
                // multiple plugin entries exist to help disambiguation) or "MSB:x / LSB:y"
                char sliderLabel[256] = {};
                if (boundName) {
                    char* dst = sliderLabel;
                    // Prefix "[G:N] " when ≥2 plugins share the track
                    if (nrpnEntries.size() >= 2) {
                        int prefixLen = snprintf(dst, sizeof(sliderLabel), "[G:%u] ", ae.umpGroup);
                        if (prefixLen > 0) dst += prefixLen;
                    }
                    const char* src = boundName;
                    while (*src && dst < sliderLabel + sizeof(sliderLabel) - 2) {
                        if (*src == '%') *dst++ = '%';
                        *dst++ = *src++;
                    }
                } else {
                    snprintf(sliderLabel, sizeof(sliderLabel), "MSB:%d / LSB:%d",
                             ae.paramIndex >> 7, ae.paramIndex & 0x7F);
                }

                const bool hasPickerBtn = !nrpnEntries.empty();
                const float frameH  = ImGui::GetFrameHeight();
                const float spacing = ImGui::GetStyle().ItemSpacing.x;
                const float nBtns   = hasPickerBtn ? 3.0f : 2.0f;
                const float sliderW = std::max(
                    ImGui::GetContentRegionAvail().x - nBtns * (frameH + spacing),
                    20.0f * uiScale);

                // [−] decrement
                if (ImGui::Button("-##dec", ImVec2(frameH, frameH)))
                    if (ae.paramIndex > 0) { --ae.paramIndex; state.dirtyAfterEdit = true; }
                ImGui::SameLine();

                // Slider (0 – 16383 combined NRPN index)
                int combined = static_cast<int>(ae.paramIndex);
                ImGui::SetNextItemWidth(sliderW);
                if (ImGui::SliderInt("##nrpn", &combined, 0, 16383, sliderLabel)) {
                    ae.paramIndex = static_cast<uint16_t>(combined);
                    state.dirtyAfterEdit = true;
                }
                ImGui::SameLine();

                // [+] increment
                if (ImGui::Button("+##inc", ImVec2(frameH, frameH)))
                    if (ae.paramIndex < 16383) { ++ae.paramIndex; state.dirtyAfterEdit = true; }

                // [▼] picker arrow (NRPN only, when entries are available)
                if (hasPickerBtn) {
                    ImGui::SameLine();
                    if (contextActionArrowButton("##nrpnpick", ImGuiDir_Down))
                        ImGui::OpenPopup("##nrpnpicker");
                    if (renderNrpnPicker("##nrpnpicker", ae.paramIndex, ae.umpGroup,
                            state.nrpnPickerHoveredPlugin, nrpnEntries))
                        state.dirtyAfterEdit = true;
                }
            } else {
                ImGui::TextDisabled("—");
            }

            // Value
            ImGui::TableSetColumnIndex(4);
            const bool isCC = (ae.type == T::ControlChange);
            if (isCC && isSwitchCC(ae.paramIndex)) {
                int onOff = ae.normalizedValue >= 0.5 ? 1 : 0;
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::Combo("##val", &onOff, "Off\0On\0")) {
                    ae.normalizedValue = onOff ? 1.0 : 0.0;
                    state.dirtyAfterEdit = true;
                }
            } else {
                float v = static_cast<float>(ae.normalizedValue);
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::SliderFloat("##val", &v, 0.0f, 1.0f, "%.4f"))
                    ae.normalizedValue = static_cast<double>(v);
                if (ImGui::IsItemDeactivatedAfterEdit())
                    state.dirtyAfterEdit = true;
            }

            ImGui::PopStyleColor();
            ImGui::PopID();
        }

        ImGui::EndTable();
    }

    // Apply deferred row operations (must be outside the table loop)
    if (pendingInsertNote >= 0 &&
            pendingInsertNote <= static_cast<int>(note.automationEvents.size())) {
        const auto& ref = note.automationEvents[pendingInsertNote];
        ClipPreview::AutomationEvent newEvt{};
        newEvt.timeSeconds     = ref.timeSeconds;
        newEvt.normalizedValue = 0.0;
        newEvt.type            = ClipPreview::AutomationEvent::Type::ControlChange;
        newEvt.channel         = ref.channel;
        newEvt.noteNumber      = ref.noteNumber;
        newEvt.umpGroup        = ref.umpGroup;
        newEvt.rawEventIdx     = SIZE_MAX; // synthetic — no original raw event
        note.automationEvents.insert(
            note.automationEvents.begin() + pendingInsertNote, std::move(newEvt));
        state.dirtyAfterEdit = true;
    }
    if (pendingDelete >= 0) {
        state.deletedRawIdxs.push_back(note.automationEvents[pendingDelete].rawEventIdx);
        note.automationEvents.erase(note.automationEvents.begin() + pendingDelete);
        state.dirtyAfterEdit = true;
    }
    if (pendingInsertClip >= 0 &&
            pendingInsertClip <= static_cast<int>(state.editClipEvents.size())) {
        const auto& ref = state.editClipEvents[pendingInsertClip];
        ClipPreview::AutomationEvent newEvt{};
        newEvt.timeSeconds     = ref.timeSeconds;
        newEvt.normalizedValue = 0.0;
        newEvt.type            = ClipPreview::AutomationEvent::Type::ControlChange;
        newEvt.channel         = ref.channel;
        newEvt.umpGroup        = ref.umpGroup;
        newEvt.rawEventIdx     = SIZE_MAX; // synthetic — no original raw event
        state.editClipEvents.insert(
            state.editClipEvents.begin() + pendingInsertClip, std::move(newEvt));
        state.dirtyAfterEdit = true;
    }
    if (pendingDeleteClip >= 0) {
        state.deletedRawIdxs.push_back(state.editClipEvents[pendingDeleteClip].rawEventIdx);
        state.editClipEvents.erase(state.editClipEvents.begin() + pendingDeleteClip);
        state.dirtyAfterEdit = true;
    }

}

// ── main window ───────────────────────────────────────────────────────────────

void PianoRollEditor::renderWindow(WindowState& state, const RenderContext& ctx) {
    const float uiScale = ctx.uiScale;
    std::string title = std::format("Piano Roll - {}###PianoRoll_{}_{}",
                                     state.clipName, state.trackIndex, state.clipId);

    ImGui::SetNextWindowSize({920.0f * uiScale, 520.0f * uiScale}, ImGuiCond_FirstUseEver);

    // Prevent the window from being dragged while the user is interacting with its
    // content (note drag, piano key preview).  We must decide this *before* Begin()
    // because that is where ImGui processes title-bar dragging.  state.drag.active and
    // state.previewNote are set later in the same frame (inside the child windows), so
    // they are always one frame behind.  As a zero-latency supplement we also check
    // whether LMB is already held inside the content area using last-frame bounds.
    bool lockWindowMove = state.drag.active || state.previewNote >= 0;
    if (!lockWindowMove && ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
            state.lastWindowSize.x > 0.0f) {
        // Title bar occupies approximately one frame-height at the top of the window.
        const float titleH = ImGui::GetFrameHeight();
        const ImVec2 mp    = ImGui::GetIO().MousePos;
        if (mp.x >= state.lastWindowPos.x &&
            mp.x <  state.lastWindowPos.x + state.lastWindowSize.x &&
            mp.y >= state.lastWindowPos.y + titleH &&
            mp.y <  state.lastWindowPos.y + state.lastWindowSize.y)
            lockWindowMove = true;
    }

    const ImGuiWindowFlags extraFlags = lockWindowMove ? ImGuiWindowFlags_NoMove : 0;
    if (!ImGui::Begin(title.c_str(), &state.visible, extraFlags)) {
        ImGui::End();
        return;
    }

    renderControls(state, uiScale);
    ImGui::Separator();

    const float pianoW   = kPianoKeyWidth * uiScale;
    const float noteH    = state.view.vZoom * uiScale;
    const float pxPerSec = state.view.hZoom * uiScale;
    const float rulerH   = kRulerHeight * uiScale;

    double clipDuration = state.preview ? std::max(0.01, state.preview->clipDurationSeconds) : 10.0;
    // Canvas width: at least 1 hour at the current zoom so the user can always scroll
    // freely without hitting a hard right boundary, just like the timeline track.
    const float totalNoteW = std::max(
        static_cast<float>(clipDuration + std::max(4.0, clipDuration * 0.5)),
        3600.0f) * pxPerSec;

    const float scrollbarSize  = ImGui::GetStyle().ScrollbarSize;
    const ImVec2 avail         = ImGui::GetContentRegionAvail();

    // Reserve bottom panel height; rest goes to the piano roll.
    const float panelH   = std::min(180.0f * uiScale, avail.y * 0.30f);
    const float mainAreaH = avail.y - panelH - ImGui::GetStyle().ItemSpacing.y;

    // Clamp vertical scroll
    const float noteAreaH     = mainAreaH - rulerH - scrollbarSize;
    const float maxVScrollNote = std::max(0.0f,
        static_cast<float>(kNoteCount) - noteAreaH / noteH);
    state.view.vScrollNote = std::clamp(state.view.vScrollNote, 0.0f, maxVScrollNote);
    const float vScrollPx = state.view.vScrollNote * noteH;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,   {0.0f, 0.0f});

    // ── Piano Key Child (left, fixed width) ──────────────────────────────────
    ImGui::BeginChild("##PianoKeys", {pianoW, mainAreaH}, false,
                       ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                       ImGuiWindowFlags_NoBackground);
    {
        ImVec2 orig = ImGui::GetWindowPos();
        float  w    = ImGui::GetWindowSize().x;
        float  h    = ImGui::GetWindowSize().y;
        renderPianoKeys(ImGui::GetWindowDrawList(), orig, w, h, noteH, vScrollPx, uiScale,
                        state.previewNote);

        // ── Scroll forwarding: wheel over the key strip scrolls the note grid ─
        if (ImGui::IsWindowHovered()) {
            const float wheel = ImGui::GetIO().MouseWheel;
            if (std::abs(wheel) > 0.0001f) {
                state.view.vScrollNote -= wheel * 3.0f;
                state.view.vScrollNote  = std::clamp(state.view.vScrollNote, 0.0f, maxVScrollNote);
            }
        }

        // ── Piano key interaction (live note preview) ─────────────────────
        const float pkRulerH  = kRulerHeight * uiScale;
        const float noteAreaY = orig.y + pkRulerH;
        const float noteAreaH = h - pkRulerH;
        // Suppress key-press preview while the user is scrolling.
        const bool scrolling = std::abs(ImGui::GetIO().MouseWheel)  > 0.0001f ||
                               std::abs(ImGui::GetIO().MouseWheelH) > 0.0001f;
        if (noteAreaH > 0.0f && noteH > 0.0f) {
            const ImVec2 mp       = ImGui::GetIO().MousePos;
            // Determine which MIDI note is under the cursor (may be -1 if outside).
            int hoverNote = -1;
            if (mp.x >= orig.x && mp.x < orig.x + w &&
                    mp.y >= noteAreaY && mp.y < orig.y + h) {
                int slot = static_cast<int>((mp.y - noteAreaY + vScrollPx) / noteH);
                slot = std::clamp(slot, 0, kNoteCount - 1);
                hoverNote = (kNoteCount - 1) - slot;
            }

            if (ImGui::IsWindowHovered() && hoverNote >= 0 && !scrolling)
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

            // Press: start preview note (suppressed while scrolling).
            if (!scrolling && ImGui::IsWindowHovered() && hoverNote >= 0 &&
                    ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                if (ctx.previewNoteOn) ctx.previewNoteOn(state.trackIndex, hoverNote);
                state.previewNote = hoverNote;
            }
            // Slide: move to adjacent key while holding.
            else if (!scrolling && state.previewNote >= 0 &&
                         ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
                         hoverNote >= 0 && hoverNote != state.previewNote) {
                if (ctx.previewNoteOff) ctx.previewNoteOff(state.trackIndex, state.previewNote);
                if (ctx.previewNoteOn)  ctx.previewNoteOn(state.trackIndex, hoverNote);
                state.previewNote = hoverNote;
            }
        }
        // Release is unconditional so it fires even if cursor left the strip.
        if (state.previewNote >= 0 && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            if (ctx.previewNoteOff) ctx.previewNoteOff(state.trackIndex, state.previewNote);
            state.previewNote = -1;
        }
    }
    ImGui::EndChild();

    ImGui::SameLine(0.0f, 0.0f);

    // ── Note Grid Child (right, H-scrollable, V-manual) ──────────────────────
    ImGui::BeginChild("##NoteGrid", {0.0f, mainAreaH}, false,
                       ImGuiWindowFlags_HorizontalScrollbar |
                       ImGuiWindowFlags_NoScrollWithMouse  |
                       ImGuiWindowFlags_NoBackground);
    {
        // Scroll handling (NoScrollWithMouse suppresses ImGui's own wheel
        // handling, so we drive both axes manually here).
        //   plain wheel           → vertical scroll (vScrollNote)
        //   shift + wheel         → horizontal scroll
        //   trackpad H gesture    → horizontal scroll
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
            const float wheel  = ImGui::GetIO().MouseWheel;
            const float wheelH = ImGui::GetIO().MouseWheelH;
            const bool  shift  = ImGui::GetIO().KeyShift;

            if (std::abs(wheelH) > 0.0001f) {
                ImGui::SetScrollX(ImGui::GetScrollX() - wheelH * 40.0f * uiScale);
            } else if (std::abs(wheel) > 0.0001f && shift) {
                ImGui::SetScrollX(ImGui::GetScrollX() - wheel * 40.0f * uiScale);
            } else if (std::abs(wheel) > 0.0001f) {
                state.view.vScrollNote -= wheel * 3.0f;
                state.view.vScrollNote = std::clamp(state.view.vScrollNote, 0.0f, maxVScrollNote);
            }
        }

        // Define H scroll range (only width matters — we control V manually)
        ImGui::SetCursorPos({0.0f, 0.0f});
        ImGui::Dummy({totalNoteW, 1.0f});

        float hScroll = ImGui::GetScrollX();
        float visW    = ImGui::GetWindowSize().x;
        float visH    = ImGui::GetWindowSize().y - scrollbarSize;
        if (visW < 0.0f) visW = 0.0f;
        if (visH < 0.0f) visH = 0.0f;

        renderNoteGrid(ImGui::GetWindowDrawList(), ImGui::GetWindowPos(),
                       visW, visH, noteH, pxPerSec, hScroll, vScrollPx,
                       state, uiScale);
    }
    ImGui::EndChild();

    ImGui::PopStyleVar(2);

    // ── Automation Panel (bottom) ─────────────────────────────────────────────
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::BeginChild("##AutoPanel", {0.0f, panelH - ImGui::GetStyle().ItemSpacing.y - 2.0f},
                       false, ImGuiWindowFlags_None);
    renderAutomationPanel(state, ctx);
    ImGui::EndChild();

    // ── Delete-note confirmation popup ────────────────────────────────────────
    if (state.needsDeletePopup) {
        ImGui::OpenPopup("Delete Note?##pr");
        state.needsDeletePopup = false;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                            ImGuiCond_Appearing, {0.5f, 0.5f});
    if (ImGui::BeginPopupModal("Delete Note?##pr", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Delete the selected note?");
        ImGui::Spacing();
        if (ImGui::Button("Delete", {120.0f * uiScale, 0.0f})) {
            const int idx = state.noteToDeleteIdx;
            if (idx >= 0 && idx < static_cast<int>(state.editNotes.size())) {
                state.editNotes[idx].deleted = true;
                if (state.selectedNoteIdx == idx) state.selectedNoteIdx = -1;
                state.dirtyAfterEdit = true;
            }
            state.noteToDeleteIdx = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", {120.0f * uiScale, 0.0f})) {
            state.noteToDeleteIdx = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // ── Write-back ───────────────────────────────────────────────────────────
    if (state.dirtyAfterEdit)
        applyNoteEdits(state, ctx);

    // Store bounds so the next frame can hit-test before Begin().
    state.lastWindowPos  = ImGui::GetWindowPos();
    state.lastWindowSize = ImGui::GetWindowSize();

    ImGui::End();
}

} // namespace uapmd::gui
