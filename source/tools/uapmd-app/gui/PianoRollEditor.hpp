#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <unordered_set>

#include <imgui.h>

#include "ClipPreview.hpp"

namespace uapmd_app_gui {

// Extended note type used inside the piano-roll editor.
// Inherits all display fields from ClipPreview::MidiNote and adds the
// per-note automation event list, which is populated by the editor itself
// from rawMidiData (ClipPreview no longer stores automation events).
using EditNote = uapmd_app::PianoRollEditNote;

// Editable piano roll view for a MIDI clip.
// Opens like MidiDumpWindow — one window per (trackIndex, clipId) pair.
// Supports:
//   - Horizontal / vertical scroll and zoom
//   - Note drag-to-move with configurable snap grid
//   - Per-note and channel-level automation event list
class PianoRollEditor {
public:
    // One entry per plugin instance on the track; contains all addressable parameters.
    struct PluginParamEntry {
        int32_t     instanceId{-1};
        std::string pluginName;
        uint8_t     group{0};   // UMP group (0–15) assigned to this instance
        struct Param {
            uint16_t    nrpnIndex{0}; // (bank << 7) | lsb — 14-bit NRPN address
            std::string path;
            std::string name;
        };
        std::vector<Param> params;
    };

    // Passed each frame to render(); carries per-frame scale and write-back callbacks.
    struct RenderContext {
        float uiScale{1.0f};
        std::function<void(int32_t trackIndex, int32_t clipId)> onCommitted;
        // Called when the user presses/slides on a piano key (for live note preview).
        std::function<void(int32_t trackIndex, int midiNote)> previewNoteOn;
        std::function<void(int32_t trackIndex, int midiNote)> previewNoteOff;
        // Returns one PluginParamEntry per plugin instance on the track.
        // Used to populate the NRPN "plugin param" picker. May return empty.
        std::function<std::vector<PluginParamEntry>(int32_t trackIndex)> getTrackPluginParameters;
    };

    void showClip(int32_t trackIndex, int32_t clipId,
                  const std::string& clipName,
                  std::shared_ptr<ClipPreview> preview);

    // Refreshes an already-open clip after an external history mutation while
    // preserving its window and view state.
    void reloadClip(int32_t trackIndex, int32_t clipId,
                    std::shared_ptr<ClipPreview> preview);

    void render(const RenderContext& ctx);

private:
    struct ViewState {
        float visibleBeats{16.0f}; // Number of beats across the note grid
        float lastPxPerSec{0.0f};  // Preserves the viewed time when the scale changes
        float vZoom{12.0f};       // Pixels per note row at uiScale=1
        float hScrollPx{0.0f};    // Horizontal scroll in pixels
        float vScrollNote{0.0f};  // Vertical scroll in note-slot units (0=top/note127)
        bool  rowScrollbarDragging{false};
        float rowScrollbarDragOffset{0.0f};
        bool  timelineScrollbarDragging{false};
        float timelineScrollbarDragOffset{0.0f};
        int   snapIdx{2};         // Defaults to 1/16; options are defined in PianoRollEditor.cpp.
    };

    // Tracks an in-flight note drag across frames.
    struct DragState {
        enum class Mode { Move, ResizeLeft, ResizeRight };
        bool   active{false};
        Mode   mode{Mode::Move};
        int    noteIdx{-1};
        float  startMouseX{0.f}; // Screen-space cursor X at the moment drag began
        float  startMouseY{0.f}; // Screen-space cursor Y at the moment drag began
        double origStartSec{0.0};
        double origEndSec{0.0};  // origStartSec + origDuration (for resize)
        int    origNoteNum{0};
        std::vector<std::pair<int, EditNote>> notes;
    };

    using NoteAction = uapmd_app::PianoRollSession::Action;

    struct WindowState {
        int32_t trackIndex{-1};
        int32_t clipId{-1};
        std::string clipName;
        bool visible{false};
        std::shared_ptr<ClipPreview> preview;
        std::shared_ptr<uapmd_app::PianoRollSession> document;
        ViewState view;
        NoteAction pending_action{NoteAction::None};
        double paste_seconds{0.0};
        bool marquee_active{false};
        bool marquee_additive{false};
        ImVec2 marquee_anchor;
        bool long_press_opened{false};
        DragState drag;
        int  previewNote{-1};       // MIDI note currently sounding via piano-key click (-1 = none)
        // Delete-note confirmation state
        int  noteToDeleteIdx{-1};   // index into editNotes of the note awaiting confirmation
        bool needsDeletePopup{false}; // triggers ImGui::OpenPopup on the next renderWindow frame
        // Used to hit-test the content area before Begin() to lock window movement.
        ImVec2 lastWindowPos{0.0f, 0.0f};
        ImVec2 lastWindowSize{0.0f, 0.0f};
        // Last-hovered plugin index inside the NRPN parameter picker popup.
        int nrpnPickerHoveredPlugin{0};
    };

    std::map<std::pair<int32_t, int32_t>, WindowState> windows_;

    static constexpr float kPianoKeyWidth = 48.0f; // base px at uiScale=1
    static constexpr float kRulerHeight   = 20.0f; // base px at uiScale=1
    static constexpr int   kNoteCount     = 128;

    void renderWindow(WindowState& state, const RenderContext& ctx);
    void renderControls(WindowState& state, float uiScale);
    bool applyNoteEdits(WindowState& state, const RenderContext& ctx);
    static void renderNoteActions(WindowState& state);
    static void performNoteAction(WindowState& state);

    // Piano key column (left strip, V-synced with note grid)
    // previewNote: MIDI note to highlight as pressed (-1 = none)
    void renderPianoKeys(ImDrawList* dl, ImVec2 origin, float width, float height,
                         float noteH, float vScrollPx, float uiScale, int previewNote) const;

    // Note grid (right area, H+V scrollable)
    // Returns true if a note was clicked (selectedNoteIdx updated).
    void renderNoteGrid(ImDrawList* dl, ImVec2 origin, float width, float height,
                        float noteH, float pxPerSec, float hScroll, float vScrollPx,
                        WindowState& state, float uiScale) const;

    // Automation event list for the selected note (bottom panel)
    void renderAutomationPanel(WindowState& state, const RenderContext& ctx) const;

    // Parses automation events (CC, RPN, NRPN, pitch-bend, pressure, per-note
    // controllers) from rawMidiData into editNotes[*].automationEvents and
    // clipEvents.  Called by showClip and after a write-back reload.

    // Two-pane NRPN parameter picker popup (no trigger button — caller must call
    // ImGui::OpenPopup(popupId) before this). Left pane shows plugin names (with
    // group prefix "[N] Name"); right pane is a scrollable 3-column table.
    // Returns true when a parameter is picked; umpGroup is set to the selected entry's group.
    // popupId must be unique per call site (scoped under the current ImGui ID stack).
    static bool renderNrpnPicker(const char* popupId,
                                  uint16_t& paramIndex,
                                  uint8_t&  umpGroup,
                                  int& hoveredPlugin,
                                  const std::vector<PluginParamEntry>& entries);

    static bool isBlackKey(int midiNote) noexcept;
    static const char* noteNameCStr(int midiNote) noexcept;
    static std::string fullNoteName(int midiNote);
    static const char* automationTypeName(ClipPreview::AutomationEvent::Type t) noexcept;
};

} // namespace uapmd_app_gui
