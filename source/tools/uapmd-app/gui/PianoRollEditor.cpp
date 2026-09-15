#include "PianoRollEditor.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>
#include <limits>
#include <unordered_map>

#include "ContextActions.hpp"
#include "FontIcons.hpp"

#include <imgui.h>
#include <umppi/umppi.hpp>


namespace uapmd_app_gui {

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
    // Octave labels sit on the white piano keys, so use a dark key label
    // color rather than the generally light UI text color.
    theme.key_label = ImGui::GetColorU32(
        mixColor(ImVec4(0.10f, 0.10f, 0.12f, 1.0f), editor_bg, dark ? 0.20f : 0.05f));
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
    state.document = uapmd_app::AppModel::instance().openPianoRollSession(trackIndex, clipId);
    if (!state.preview || !state.preview->rawMidiData ||
            !state.document->matchesSource(*state.preview->rawMidiData))
        state.document->loadNotes(
            state.preview ? state.preview->midiNotes : std::vector<ClipPreview::MidiNote>{},
            state.preview ? state.preview->rawMidiData : nullptr,
            state.preview ? state.preview->clipDurationSeconds : 0.01);
    state.pending_action = NoteAction::None;
    state.marquee_active = false;
    state.noteToDeleteIdx = -1;
    state.needsDeletePopup = false;
    state.drag = DragState{};

    // On first open (vScrollNote still at its default 0), scroll so the
    // pitch range of the clip is centred in the visible area.
    if (state.view.vScrollNote == 0.0f) {
        // Centre the view on the note range of the clip; fall back to C4 (60)
        // when the clip is empty so we don't open at note 127 / C9.
        const float midNote = (!state.document->editNotes.empty())
                              ? (state.preview->minNote + state.preview->maxNote) * 0.5f
                              : 60.0f;
        float midIdx = static_cast<float>(kNoteCount - 1) - midNote;
        state.view.vScrollNote = std::max(0.0f, midIdx - 8.0f);
    }
}

void PianoRollEditor::reloadClip(int32_t trackIndex, int32_t clipId,
                                 std::shared_ptr<ClipPreview> preview) {
    const auto key = std::make_pair(trackIndex, clipId);
    const auto it = windows_.find(key);
    if (it == windows_.end() || !it->second.visible)
        return;
    auto& state = it->second;
    if (state.document && preview && preview->rawMidiData &&
            state.document->matchesSource(*preview->rawMidiData))
        return;
    showClip(trackIndex, clipId, state.clipName, std::move(preview));
}

void PianoRollEditor::render(const RenderContext& ctx) {
    for (auto& [key, state] : windows_)
        if (state.visible)
            renderWindow(state, ctx);

    std::erase_if(windows_, [](const auto& p) {
        if (p.second.visible)
            return false;
        uapmd_app::AppModel::instance().closePianoRollSession(p.first.first, p.first.second);
        return true;
    });
}

// ── static helpers ────────────────────────────────────────────────────────────

bool PianoRollEditor::applyNoteEdits(WindowState& state, const RenderContext& ctx) {
    if (!state.document->commit(uapmd_app::AppModel::instance()))
        return false;
    state.preview->rawMidiData = state.document->rawMidiData;
    state.preview->midiNotes.clear();
    for (const auto& note : state.document->editNotes)
        state.preview->midiNotes.push_back(static_cast<const ClipPreview::MidiNote&>(note));
    state.preview->minNote = state.document->minNote;
    state.preview->maxNote = state.document->maxNote;
    state.preview->clipDurationSeconds = state.document->clipDurationSeconds;
    state.drag = DragState{};
    state.noteToDeleteIdx = -1;
    if (ctx.onCommitted)
        ctx.onCommitted(state.trackIndex, state.clipId);
    return true;
}

// ── controls bar ─────────────────────────────────────────────────────────────

void PianoRollEditor::renderNoteActions(WindowState& state) {
    ImGui::TextDisabled("%zu notes selected", state.document->selected_notes.size());
    ImGui::BeginDisabled(state.document->selected_notes.empty());
    if (contextActionMenuItem("Cut"))
        state.pending_action = NoteAction::Cut;
    if (contextActionMenuItem("Copy"))
        state.pending_action = NoteAction::Copy;
    if (contextActionMenuItem("Delete"))
        state.pending_action = NoteAction::Delete;
    ImGui::EndDisabled();
    ImGui::BeginDisabled(state.document->clipboard.empty());
    if (contextActionMenuItem("Paste here"))
        state.pending_action = NoteAction::Paste;
    ImGui::EndDisabled();
    if (contextActionMenuItem("Select All Notes"))
        state.pending_action = NoteAction::SelectAll;
    ImGui::Separator();
}

void PianoRollEditor::performNoteAction(WindowState& state) {
    const auto action = std::exchange(state.pending_action, NoteAction::None);
    state.document->performAction(action, state.paste_seconds);
}

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
                            static_cast<int>(state.document->editNotes.size()),
                            static_cast<int>(state.document->editClipEvents.size()));
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
                // A pressed key represents the whole note row.  Fill the
                // complete key column so black-key preview regions do not
                // look like detached short bars.
                const float keyW = isPreview ? width : blackKeyW;
                dl->AddRectFilled({origin.x, y0}, {origin.x + keyW, y1 - 0.5f}, keyCol);
            } else {
                // Use the same pressed-note color for both key types.  The
                // white key base must not make its preview appear washed out.
                ImU32 keyCol = isPreview ? theme.key_preview_black : theme.key_white;
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
    // The grid edits clip-local time using the same reference tempo as write-back.
    const double bpm = state.preview && state.preview->rawMidiData && state.preview->rawMidiData->clipTempo > 0.0
        ? state.preview->rawMidiData->clipTempo : 120.0;

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

    const auto pointer = ImGui::GetMousePos();
    const bool inGrid = pointer.x >= origin.x && pointer.x < origin.x + width &&
        pointer.y >= noteAreaY && pointer.y < noteAreaY + noteAreaH;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        state.long_press_opened = false;
    const bool longPress = inGrid && ImGui::IsWindowHovered() && !state.long_press_opened &&
        ImGui::GetIO().MouseDownDuration[ImGuiMouseButton_Left] >= 0.5f &&
        !ImGui::IsMouseDragging(ImGuiMouseButton_Left);
    const bool cancelDrag = state.drag.active && ImGui::IsKeyPressed(ImGuiKey_Escape);
    if (longPress || cancelDrag) {
        if (longPress)
            state.long_press_opened = true;
        state.document->restoreNotes(state.drag.notes);
        state.drag = DragState{};
        state.marquee_active = false;
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

            if (ImGui::IsMouseDragging(ImGuiMouseButton_Left) && state.drag.noteIdx >= 0 &&
                    state.drag.noteIdx < static_cast<int>(state.document->editNotes.size())) {
                if (state.drag.mode == DragState::Mode::Move) {
                    double timeDelta = static_cast<double>(dx) / pxPerSec;
                    if (snapSec > 0.0 && dx != 0.0f)
                        timeDelta = std::round((state.drag.origStartSec + timeDelta) / snapSec) * snapSec - state.drag.origStartSec;
                    const int pitchDelta = -static_cast<int>(std::round(static_cast<double>(dy) / noteH));
                    state.document->moveNotes(state.drag.notes, timeDelta, pitchDelta);
                } else if (state.drag.mode == DragState::Mode::ResizeRight) {
                    double newEnd = state.drag.origEndSec + static_cast<double>(dx) / pxPerSec;
                    if (snapSec > 0.0) newEnd = std::round(newEnd / snapSec) * snapSec;
                    state.document->resizeNote(state.drag.noteIdx, state.drag.origStartSec,
                        std::max(kMinNoteDuration, newEnd - state.drag.origStartSec),
                        static_cast<uint8_t>(state.drag.origNoteNum));
                } else { // ResizeLeft
                    double newStart = state.drag.origStartSec + static_cast<double>(dx) / pxPerSec;
                    if (snapSec > 0.0) newStart = std::round(newStart / snapSec) * snapSec;
                    newStart       = std::clamp(newStart, 0.0, state.drag.origEndSec - kMinNoteDuration);
                    state.document->resizeNote(state.drag.noteIdx, newStart,
                        state.drag.origEndSec - newStart,
                        static_cast<uint8_t>(state.drag.origNoteNum));
                }
            }
        } else {
            // Mouse released — finalise drag; schedule write-back if anything changed.
            state.document->finishNoteDrag(state.drag.noteIdx, state.drag.origStartSec,
                state.drag.origEndSec, static_cast<uint8_t>(state.drag.origNoteNum));
            state.drag = DragState{};
        }
    }

    // ── Note rectangles ───────────────────────────────────────────────────────
    // Edge threshold (px) within which dragging resizes instead of moves.
    static constexpr float kResizeEdgePx = 8.0f;
    if (noteAreaH > 0.0f) {
        // Capture click intent before the loop; consumed by the first hit note.
        bool   mouseClick = inGrid && !state.drag.active &&
                            ImGui::IsWindowHovered() &&
                            ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        bool   mouseRightClick = inGrid && !state.drag.active && ImGui::IsWindowHovered() &&
            (longPress || ImGui::IsMouseClicked(ImGuiMouseButton_Right));
        const auto& io = ImGui::GetIO();
        const bool shortcuts = inGrid && ImGui::IsWindowHovered() && !io.WantTextInput &&
            !state.drag.active && !state.marquee_active && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
        const bool pasteKey = shortcuts && (io.KeyCtrl || io.KeySuper) && ImGui::IsKeyPressed(ImGuiKey_V, false);
        if (shortcuts) {
            if (io.KeyCtrl || io.KeySuper) {
                if (ImGui::IsKeyPressed(ImGuiKey_A, false)) state.pending_action = NoteAction::SelectAll;
                if (ImGui::IsKeyPressed(ImGuiKey_C, false)) state.pending_action = NoteAction::Copy;
                if (ImGui::IsKeyPressed(ImGuiKey_X, false)) state.pending_action = NoteAction::Cut;
                if (pasteKey) state.pending_action = NoteAction::Paste;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false))
                state.pending_action = NoteAction::Delete;
        }
        if (mouseRightClick)
            state.marquee_active = false;
        if (mouseRightClick || pasteKey) {
            const float snapBeats = kSnapValues[std::clamp(state.view.snapIdx, 0, 6)];
            const double snapSeconds = snapBeats * 60.0 / bpm;
            double seconds = (pointer.x - origin.x + hScroll) / pxPerSec;
            if (snapSeconds > 0.0)
                seconds = std::round(seconds / snapSeconds) * snapSeconds;
            state.paste_seconds = std::max(0.0, seconds);
        }
        ImVec2 mousePos   = ImGui::GetMousePos();
        bool   hoverNote  = false;
        bool   hoverEdge  = false;

        for (int ni = 0; ni < static_cast<int>(state.document->editNotes.size()); ++ni) {
            const auto& note = state.document->editNotes[ni];
            if (note.deleted) continue;

            int noteIdx = (kNoteCount - 1) - static_cast<int>(note.note);

            float y0 = noteAreaY + noteIdx * noteH - vScrollPx;
            float y1 = y0 + noteH - 1.0f;
            float x0 = origin.x + static_cast<float>(note.startSeconds) * pxPerSec - hScroll;
            float x1 = x0 + std::max(2.0f * uiScale,
                                      static_cast<float>(note.durationSeconds) * pxPerSec);

            if (x1 < origin.x || x0 > origin.x + width) continue;
            if (y1 < noteAreaY || y0 > noteAreaY + noteAreaH) continue;

            const bool  selected = state.document->selected_notes.contains(note.edit_id);
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
                const auto& io = ImGui::GetIO();
                const bool toggle = io.KeyCtrl || io.KeySuper;
                if (toggle || io.KeyShift || !selected)
                    state.document->selectNote(ni, toggle || io.KeyShift, toggle);
                else
                    state.document->selectedNoteIdx = ni;
                state.drag.active = !toggle && !io.KeyShift;
                state.drag.notes.clear();
                if (state.drag.active)
                    for (int i = 0; i < static_cast<int>(state.document->editNotes.size()); ++i)
                        if (!state.document->editNotes[i].deleted && state.document->selected_notes.contains(state.document->editNotes[i].edit_id))
                            state.drag.notes.emplace_back(i, state.document->editNotes[i]);
                state.drag.noteIdx      = ni;
                state.drag.startMouseX  = ImGui::GetIO().MousePos.x;
                state.drag.startMouseY  = ImGui::GetIO().MousePos.y;
                state.drag.origStartSec = note.startSeconds;
                state.drag.origEndSec   = note.startSeconds + note.durationSeconds;
                state.drag.origNoteNum  = static_cast<int>(note.note);
                if (state.document->selected_notes.size() > 1) state.drag.mode = DragState::Mode::Move;
                else if (atRightEdge) state.drag.mode = DragState::Mode::ResizeRight;
                else if (atLeftEdge)  state.drag.mode = DragState::Mode::ResizeLeft;
                else                  state.drag.mode = DragState::Mode::Move;
                mouseClick = false; // consume so only the top-most note is picked
            }
            if (mouseRightClick && overNote) {
                if (!state.document->selected_notes.contains(note.edit_id))
                    state.document->selectNote(ni);
                else
                    state.document->selectedNoteIdx = ni;
                ImGui::OpenPopup("##note_editor");
                mouseRightClick = false;
            }
        }

        if (mouseRightClick)
            ImGui::OpenPopup("##note_editor");
        if (ImGui::BeginPopup("##note_editor")) {
            renderNoteActions(state);
            const int idx = state.document->selectedNoteIdx;
            if (idx < 0 || idx >= static_cast<int>(state.document->editNotes.size()) ||
                    state.document->editNotes[idx].deleted) {
                ImGui::TextDisabled("No note selected.");
            } else {
                auto& note = state.document->editNotes[idx];
                if (state.document->selected_notes.size() > 1)
                    ImGui::TextDisabled("Properties below apply to the primary note only.");
                ImGui::Text("%s | %.3fs", fullNoteName(note.note).c_str(), note.startSeconds);
                ImGui::Separator();

                bool noteEdited = false;
                ImGui::TextUnformatted("Velocity");
                ImGui::SetNextItemWidth(230.0f * uiScale);
                if (ImGui::SliderFloat("##velocity_slider", &note.velocity, 0.0f, 1.0f, "%.3f"))
                    noteEdited = true;
                note.velocity = std::clamp(note.velocity, 0.0f, 1.0f);

                ImGui::TextUnformatted("Note attribute type");
                uint8_t attributeType = note.attributeType;
                ImGui::SetNextItemWidth(200.0f * uiScale);
                if (ImGui::InputScalar("##attribute_type_input", ImGuiDataType_U8,
                                       &attributeType, nullptr, nullptr, "%u")) {
                    note.attributeType = std::min<uint8_t>(attributeType, 127);
                    noteEdited = true;
                }
                const ImVec2 attributeTypeMin = ImGui::GetItemRectMin();
                const ImVec2 attributeTypeMax = ImGui::GetItemRectMax();
                ImGui::SameLine();
                if (ImGui::ArrowButton("##attribute_type_dropdown", ImGuiDir_Down))
                    ImGui::OpenPopup("##attribute_type_options");
                ImGui::SetNextWindowPos(attributeTypeMin, ImGuiCond_Appearing);
                ImGui::SetNextWindowSize(ImVec2(attributeTypeMax.x - attributeTypeMin.x, 0.0f),
                                         ImGuiCond_Appearing);
                if (ImGui::BeginPopup("##attribute_type_options")) {
                    if (ImGui::Selectable("Pitch 7.9 (3)", note.attributeType == 3)) {
                        note.attributeType = 3;
                        noteEdited = true;
                    }
                    ImGui::EndPopup();
                }

                ImGui::TextUnformatted("Note attribute value");
                uint16_t attributeValue = note.attributeValue;
                const uint16_t minAttributeValue = 0;
                const uint16_t maxAttributeValue = 65535;
                ImGui::SetNextItemWidth(230.0f * uiScale);
                if (ImGui::SliderScalar("##attribute_value_slider", ImGuiDataType_U16,
                                        &attributeValue, &minAttributeValue,
                                        &maxAttributeValue, "%u")) {
                    note.attributeValue = attributeValue;
                    noteEdited = true;
                }

                if (noteEdited)
                    state.document->dirtyAfterEdit = true;
            }
            ImGui::EndPopup();
        }

        if (mouseClick && !ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            state.marquee_active = true;
            state.marquee_additive = ImGui::GetIO().KeyShift;
            state.marquee_anchor = mousePos;
        }
        if (state.marquee_active) {
            const ImVec2 end(std::clamp(pointer.x, origin.x, origin.x + width),
                             std::clamp(pointer.y, noteAreaY, noteAreaY + noteAreaH));
            const ImVec2 min(std::min(state.marquee_anchor.x, end.x), std::min(state.marquee_anchor.y, end.y));
            const ImVec2 max(std::max(state.marquee_anchor.x, end.x), std::max(state.marquee_anchor.y, end.y));
            dl->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_Header, 0.3f));
            dl->AddRect(min, max, ImGui::GetColorU32(ImGuiCol_HeaderActive));
            if (ImGui::IsKeyPressed(ImGuiKey_Escape))
                state.marquee_active = false;
            else if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                if (!state.marquee_additive)
                    state.document->selectNote(-1);
                if (max.x - min.x >= 4.0f * uiScale || max.y - min.y >= 4.0f * uiScale)
                    for (int i = 0; i < static_cast<int>(state.document->editNotes.size()); ++i) {
                        const auto& note = state.document->editNotes[i];
                        if (note.deleted)
                            continue;
                        const float x = origin.x + note.startSeconds * pxPerSec - hScroll;
                        const float y = noteAreaY + (127 - note.note) * noteH - vScrollPx;
                        if (x < max.x && x + std::max(2.0f * uiScale, static_cast<float>(note.durationSeconds) * pxPerSec) > min.x &&
                                y < max.y && y + noteH - 1.0f > min.y)
                            state.document->selectNote(i, true);
                    }
                state.marquee_active = false;
            }
        } else if (!cancelDrag && ImGui::IsWindowHovered() && ImGui::IsKeyPressed(ImGuiKey_Escape))
            state.document->selectNote(-1);

        // Cursor: EW-resize for edges, ResizeAll for body; same when drag is active.
        if (state.drag.active) {
            if (state.drag.mode != DragState::Mode::Move)
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            else
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        } else if (hoverEdge && state.document->selected_notes.size() <= 1) {
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
        if (inGrid) {
            state.marquee_active = false;
            // Find any existing, non-deleted note under the cursor.
            int hitIdx = -1;
            for (int ni = 0; ni < static_cast<int>(state.document->editNotes.size()); ++ni) {
                const auto& n = state.document->editNotes[ni];
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

                state.document->createNote(time, noteDuration,
                    static_cast<uint8_t>(std::clamp(midiNote, 0, 127)), 0.787f);
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

    const bool hasSelection = state.document->selectedNoteIdx >= 0 &&
        state.document->selectedNoteIdx < static_cast<int>(state.document->editNotes.size());

    if (!hasSelection) {
        // Show clip-level automation events
        ImGui::TextDisabled("No note selected — showing %d clip-level automation events.",
                            static_cast<int>(state.document->editClipEvents.size()));

        if (state.document->editClipEvents.empty()) return;

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

            const int evtCount = static_cast<int>(state.document->editClipEvents.size());
            for (int ci = 0; ci < evtCount; ++ci) {
                const auto& evt = state.document->editClipEvents[ci];
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
                    for (int ni = 0; ni < static_cast<int>(state.document->editNotes.size()); ++ni) {
                        const auto& n = state.document->editNotes[ni];
                        if (!n.deleted &&
                                evt.timeSeconds >= n.startSeconds &&
                                evt.timeSeconds <= n.startSeconds + n.durationSeconds) {
                            state.document->selectNote(ni);
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

    auto& note = state.document->editNotes[state.document->selectedNoteIdx];
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
        state.document->dirtyAfterEdit = true;
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
                state.document->dirtyAfterEdit = true;

            // Type
            ImGui::TableSetColumnIndex(2);
            int typeIdx = static_cast<int>(ae.type);
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::Combo("##type", &typeIdx, kAutoTypeNames, kAutoTypeCount)) {
                ae.type = static_cast<ClipPreview::AutomationEvent::Type>(typeIdx);
                state.document->dirtyAfterEdit = true;
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
                    state.document->dirtyAfterEdit = true;
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
                    if (ae.paramIndex > 0) { --ae.paramIndex; state.document->dirtyAfterEdit = true; }
                ImGui::SameLine();

                // Slider (0 – 16383 combined NRPN index)
                int combined = static_cast<int>(ae.paramIndex);
                ImGui::SetNextItemWidth(sliderW);
                if (ImGui::SliderInt("##nrpn", &combined, 0, 16383, sliderLabel)) {
                    ae.paramIndex = static_cast<uint16_t>(combined);
                    state.document->dirtyAfterEdit = true;
                }
                ImGui::SameLine();

                // [+] increment
                if (ImGui::Button("+##inc", ImVec2(frameH, frameH)))
                    if (ae.paramIndex < 16383) { ++ae.paramIndex; state.document->dirtyAfterEdit = true; }

                // [▼] picker arrow (NRPN only, when entries are available)
                if (hasPickerBtn) {
                    ImGui::SameLine();
                    if (contextActionArrowButton("##nrpnpick", ImGuiDir_Down))
                        ImGui::OpenPopup("##nrpnpicker");
                    if (renderNrpnPicker("##nrpnpicker", ae.paramIndex, ae.umpGroup,
                            state.nrpnPickerHoveredPlugin, nrpnEntries))
                        state.document->dirtyAfterEdit = true;
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
                    state.document->dirtyAfterEdit = true;
                }
            } else {
                float v = static_cast<float>(ae.normalizedValue);
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::SliderFloat("##val", &v, 0.0f, 1.0f, "%.4f"))
                    ae.normalizedValue = static_cast<double>(v);
                if (ImGui::IsItemDeactivatedAfterEdit())
                    state.document->dirtyAfterEdit = true;
            }

            ImGui::PopID();
        }

        // Channel-level events that overlap with this note's active time window (editable)
        for (int ci = 0; ci < static_cast<int>(state.document->editClipEvents.size()); ++ci) {
            auto& ae = state.document->editClipEvents[ci];
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
                state.document->dirtyAfterEdit = true;

            // Type
            ImGui::TableSetColumnIndex(2);
            int typeIdx = static_cast<int>(ae.type);
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::Combo("##type", &typeIdx, kAutoTypeNames, kAutoTypeCount)) {
                ae.type = static_cast<ClipPreview::AutomationEvent::Type>(typeIdx);
                state.document->dirtyAfterEdit = true;
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
                    state.document->dirtyAfterEdit = true;
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
                    if (ae.paramIndex > 0) { --ae.paramIndex; state.document->dirtyAfterEdit = true; }
                ImGui::SameLine();

                // Slider (0 – 16383 combined NRPN index)
                int combined = static_cast<int>(ae.paramIndex);
                ImGui::SetNextItemWidth(sliderW);
                if (ImGui::SliderInt("##nrpn", &combined, 0, 16383, sliderLabel)) {
                    ae.paramIndex = static_cast<uint16_t>(combined);
                    state.document->dirtyAfterEdit = true;
                }
                ImGui::SameLine();

                // [+] increment
                if (ImGui::Button("+##inc", ImVec2(frameH, frameH)))
                    if (ae.paramIndex < 16383) { ++ae.paramIndex; state.document->dirtyAfterEdit = true; }

                // [▼] picker arrow (NRPN only, when entries are available)
                if (hasPickerBtn) {
                    ImGui::SameLine();
                    if (contextActionArrowButton("##nrpnpick", ImGuiDir_Down))
                        ImGui::OpenPopup("##nrpnpicker");
                    if (renderNrpnPicker("##nrpnpicker", ae.paramIndex, ae.umpGroup,
                            state.nrpnPickerHoveredPlugin, nrpnEntries))
                        state.document->dirtyAfterEdit = true;
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
                    state.document->dirtyAfterEdit = true;
                }
            } else {
                float v = static_cast<float>(ae.normalizedValue);
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::SliderFloat("##val", &v, 0.0f, 1.0f, "%.4f"))
                    ae.normalizedValue = static_cast<double>(v);
                if (ImGui::IsItemDeactivatedAfterEdit())
                    state.document->dirtyAfterEdit = true;
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
        state.document->dirtyAfterEdit = true;
    }
    if (pendingDelete >= 0) {
        state.document->deletedRawIdxs.push_back(note.automationEvents[pendingDelete].rawEventIdx);
        note.automationEvents.erase(note.automationEvents.begin() + pendingDelete);
        state.document->dirtyAfterEdit = true;
    }
    if (pendingInsertClip >= 0 &&
            pendingInsertClip <= static_cast<int>(state.document->editClipEvents.size())) {
        const auto& ref = state.document->editClipEvents[pendingInsertClip];
        ClipPreview::AutomationEvent newEvt{};
        newEvt.timeSeconds     = ref.timeSeconds;
        newEvt.normalizedValue = 0.0;
        newEvt.type            = ClipPreview::AutomationEvent::Type::ControlChange;
        newEvt.channel         = ref.channel;
        newEvt.umpGroup        = ref.umpGroup;
        newEvt.rawEventIdx     = SIZE_MAX; // synthetic — no original raw event
        state.document->editClipEvents.insert(
            state.document->editClipEvents.begin() + pendingInsertClip, std::move(newEvt));
        state.document->dirtyAfterEdit = true;
    }
    if (pendingDeleteClip >= 0) {
        state.document->deletedRawIdxs.push_back(state.document->editClipEvents[pendingDeleteClip].rawEventIdx);
        state.document->editClipEvents.erase(state.document->editClipEvents.begin() + pendingDeleteClip);
        state.document->dirtyAfterEdit = true;
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
    if (!state.document->edit_error.empty()) {
        ImGui::TextWrapped("Edits were not saved: %s", state.document->edit_error.c_str());
        if (state.document->retry_available && ImGui::Button("Retry saving"))
            state.document->dirtyAfterEdit = true;
        if (ImGui::Button("Dismiss error"))
            state.document->edit_error.clear();
    }
    ImGui::Separator();

    const float pianoW   = kPianoKeyWidth * uiScale;
    const float noteH    = state.view.vZoom * uiScale;
    const float pxPerSec = state.view.hZoom * uiScale;
    const float rulerH   = kRulerHeight * uiScale;

    double clipDuration = state.preview ? std::max(0.01, state.preview->clipDurationSeconds) : 10.0;
    // Keep a modest tail after the clip so notes can be added just beyond its
    // current end without making the scrollbar thumb unusably small.
    const float totalNoteW = std::max(
        static_cast<float>(clipDuration + std::max(4.0, clipDuration * 0.5)),
        static_cast<float>(clipDuration)) * pxPerSec;

    const float scrollbarSize  = ImGui::GetStyle().ScrollbarSize;
    const ImVec2 avail         = ImGui::GetContentRegionAvail();

    // Reserve bottom panel height; rest goes to the piano roll.
    const float panelH   = std::min(180.0f * uiScale, avail.y * 0.30f);
    const float mainAreaH = avail.y - panelH - ImGui::GetStyle().ItemSpacing.y;

    // Clamp both scroll positions.
    // Both scrollbars are explicit controls.  This keeps scrolling available
    // on touch devices, where wheel input and dragging the note canvas are not
    // dependable navigation mechanisms.
    const float gridW = std::max(0.0f, avail.x - pianoW - scrollbarSize);
    const float maxHScrollPx = std::max(0.0f, totalNoteW - gridW);
    // Keep the horizontal scrollbar visible even when the whole timeline fits;
    // its full-width thumb communicates the current 0 position and preserves
    // a stable touch target as zoom or window size changes.
    const float rollAreaH = mainAreaH - scrollbarSize;
    const float noteAreaH = rollAreaH - rulerH;
    const float maxVScrollNote = std::max(0.0f,
        static_cast<float>(kNoteCount) - noteAreaH / noteH);
    state.view.vScrollNote = std::clamp(state.view.vScrollNote, 0.0f, maxVScrollNote);
    state.view.hScrollPx = std::clamp(state.view.hScrollPx, 0.0f, maxHScrollPx);

    // The shared dark theme uses a very subdued scrollbar grab.  PianoRoll
    // needs both positions to remain discoverable against its dark grid, so
    // use the app accent for the grab while retaining a dark track.
    const auto& style = ImGui::GetStyle();
    const ImVec4 scrollbarBg = mixColor(style.Colors[ImGuiCol_ScrollbarBg],
                                         style.Colors[ImGuiCol_FrameBg], 0.35f);
    const ImVec4 scrollbarGrab = style.Colors[ImGuiCol_Button];
    const ImVec4 scrollbarGrabHovered = style.Colors[ImGuiCol_ButtonHovered];
    const ImVec4 scrollbarGrabActive = style.Colors[ImGuiCol_ButtonActive];

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,   {0.0f, 0.0f});

    const float vScrollPx = state.view.vScrollNote * noteH;
    const float rollOriginX = ImGui::GetCursorPosX();

    // ── Piano Key Child (left, fixed width) ──────────────────────────────────
    ImGui::BeginChild("##PianoKeys", {pianoW, rollAreaH}, false,
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

    // ── Row scrollbar (immediately beside the keyboard) ─────────────────────
    // Keep the ruler area out of the scroll range.  The custom bar uses the
    // same note-row height as the keyboard and grid, avoiding a native child
    // scrollbar whose viewport would also include the ruler.
    ImGui::BeginChild("##PianoRollRowScrollbar", {scrollbarSize, rollAreaH}, false,
                       ImGuiWindowFlags_NoScrollbar |
                       ImGuiWindowFlags_NoBackground);
    ImGui::Dummy({scrollbarSize, rulerH});
    const ImVec2 rowBarMin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##PianoRollRows", {scrollbarSize, noteAreaH});

    const float totalRowsH = kNoteCount * noteH;
    const float rowThumbH = std::clamp(
        noteAreaH * noteAreaH / std::max(totalRowsH, noteAreaH),
        style.GrabMinSize, noteAreaH);
    const float rowThumbTravel = noteAreaH - rowThumbH;
    const float rowScrollRatio = maxVScrollNote > 0.0f
                                 ? state.view.vScrollNote / maxVScrollNote : 0.0f;
    const float rowThumbY = rowBarMin.y +
                            rowThumbTravel * std::clamp(rowScrollRatio, 0.0f, 1.0f);

    if (ImGui::IsItemActivated()) {
        const float mouseY = ImGui::GetIO().MousePos.y;
        state.view.rowScrollbarDragging = true;
        state.view.rowScrollbarDragOffset =
            (mouseY >= rowThumbY && mouseY <= rowThumbY + rowThumbH)
            ? mouseY - rowThumbY : rowThumbH * 0.5f;
    }
    if (ImGui::IsItemActive() && state.view.rowScrollbarDragging &&
            maxVScrollNote > 0.0f && rowThumbTravel > 0.0f) {
        const float thumbStart = ImGui::GetIO().MousePos.y -
                                 state.view.rowScrollbarDragOffset;
        const float ratio = std::clamp((thumbStart - rowBarMin.y) /
                                       rowThumbTravel, 0.0f, 1.0f);
        state.view.vScrollNote = ratio * maxVScrollNote;
    }
    if (!ImGui::IsItemActive())
        state.view.rowScrollbarDragging = false;

    const ImU32 rowTrackColor = ImGui::GetColorU32(scrollbarBg);
    const ImU32 rowThumbColor = ImGui::GetColorU32(
        ImGui::IsItemActive() ? scrollbarGrabActive
                              : ImGui::IsItemHovered() ? scrollbarGrabHovered
                                                       : scrollbarGrab);
    ImDrawList* rowDrawList = ImGui::GetWindowDrawList();
    rowDrawList->AddRectFilled(rowBarMin,
                               {rowBarMin.x + scrollbarSize,
                                rowBarMin.y + noteAreaH},
                               rowTrackColor, style.ScrollbarRounding);
    rowDrawList->AddRectFilled({rowBarMin.x, rowThumbY},
                               {rowBarMin.x + scrollbarSize, rowThumbY + rowThumbH},
                               rowThumbColor, style.ScrollbarRounding);
    ImGui::EndChild();

    // ── Note Grid Child (right, manually positioned in both axes) ───────────
    // The grid cannot use its own vertical scrolling because the piano-key
    // column and the grid are drawn separately and must share one offset.
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::BeginChild("##NoteGrid", {0.0f, rollAreaH}, false,
                       ImGuiWindowFlags_NoScrollWithMouse |
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
                state.view.hScrollPx -= wheelH * 40.0f * uiScale;
            } else if (std::abs(wheel) > 0.0001f && shift) {
                state.view.hScrollPx -= wheel * 40.0f * uiScale;
            } else if (std::abs(wheel) > 0.0001f) {
                state.view.vScrollNote -= wheel * 3.0f;
                state.view.vScrollNote = std::clamp(state.view.vScrollNote, 0.0f, maxVScrollNote);
            }
        }
        state.view.hScrollPx = std::clamp(state.view.hScrollPx, 0.0f, maxHScrollPx);

        // Define the H scroll range (we control both axes manually).
        ImGui::SetCursorPos({0.0f, 0.0f});
        ImGui::Dummy({totalNoteW, 1.0f});

        float hScroll = state.view.hScrollPx;
        float visW    = ImGui::GetWindowSize().x;
        float visH    = ImGui::GetWindowSize().y;
        if (visW < 0.0f) visW = 0.0f;
        if (visH < 0.0f) visH = 0.0f;

        renderNoteGrid(ImGui::GetWindowDrawList(), ImGui::GetWindowPos(),
                       visW, visH, noteH, pxPerSec, hScroll, vScrollPx,
                       state, uiScale);
    }
    ImGui::EndChild();

    // ── Timeline scrollbar (below the note grid) ────────────────────────────
    // Align it with the note grid, leaving both the keyboard and row bar blank.
    ImGui::SetCursorPosX(rollOriginX + pianoW + scrollbarSize);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, scrollbarBg);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, scrollbarGrab);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, scrollbarGrabHovered);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, scrollbarGrabActive);
    if (gridW > 0.0f) {
        const ImVec2 barMin = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##PianoRollTimelineScrollbar", {gridW, scrollbarSize});

        const float contentW = std::max(totalNoteW, gridW);
        const float thumbW = std::clamp(
            gridW * gridW / contentW, style.GrabMinSize, gridW);
        const float thumbTravel = gridW - thumbW;
        const float scrollRatio = maxHScrollPx > 0.0f
                                  ? state.view.hScrollPx / maxHScrollPx : 0.0f;
        const float thumbX = barMin.x + thumbTravel * std::clamp(scrollRatio, 0.0f, 1.0f);

        if (ImGui::IsItemActivated()) {
            const float mouseX = ImGui::GetIO().MousePos.x;
            state.view.timelineScrollbarDragging = true;
            state.view.timelineScrollbarDragOffset =
                (mouseX >= thumbX && mouseX <= thumbX + thumbW)
                ? mouseX - thumbX : thumbW * 0.5f;
        }
        if (ImGui::IsItemActive() && state.view.timelineScrollbarDragging &&
                maxHScrollPx > 0.0f && thumbTravel > 0.0f) {
            const float thumbStart = ImGui::GetIO().MousePos.x -
                                     state.view.timelineScrollbarDragOffset;
            const float ratio = std::clamp((thumbStart - barMin.x) / thumbTravel, 0.0f, 1.0f);
            state.view.hScrollPx = ratio * maxHScrollPx;
        }
        if (!ImGui::IsItemActive())
            state.view.timelineScrollbarDragging = false;

        const ImU32 trackColor = ImGui::GetColorU32(scrollbarBg);
        const ImU32 thumbColor = ImGui::GetColorU32(
            ImGui::IsItemActive() ? scrollbarGrabActive
                                  : ImGui::IsItemHovered() ? scrollbarGrabHovered
                                                           : scrollbarGrab);
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(barMin,
                                {barMin.x + gridW, barMin.y + scrollbarSize},
                                trackColor, style.ScrollbarRounding);
        drawList->AddRectFilled({thumbX, barMin.y},
                                {thumbX + thumbW, barMin.y + scrollbarSize},
                                thumbColor, style.ScrollbarRounding);
    }
    ImGui::PopStyleColor(4);

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
            state.document->deleteNote(idx);
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

    // Apply clipboard commands after drawing, when no widgets reference note storage.
    const bool batchEdit = state.pending_action == NoteAction::Cut || state.pending_action == NoteAction::Paste ||
        state.pending_action == NoteAction::Delete;
    const auto beforeNotes = batchEdit ? state.document->editNotes : std::vector<EditNote>{};
    const auto beforeSelection = batchEdit ? state.document->selected_notes : std::unordered_set<uint64_t>{};
    const int beforePrimary = state.document->selectedNoteIdx;
    performNoteAction(state);
    if (state.document->dirtyAfterEdit && !applyNoteEdits(state, ctx) && batchEdit) {
        state.document->editNotes = beforeNotes;
        state.document->selected_notes = beforeSelection;
        state.document->selectedNoteIdx = beforePrimary;
        state.document->retry_available = false;
    }

    // Store bounds so the next frame can hit-test before Begin().
    state.lastWindowPos  = ImGui::GetWindowPos();
    state.lastWindowSize = ImGui::GetWindowSize();

    ImGui::End();
}

} // namespace uapmd_app_gui
