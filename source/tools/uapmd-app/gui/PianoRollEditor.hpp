#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>

#include "ClipPreview.hpp"

namespace uapmd::gui {

// Extended note type used inside the piano-roll editor.
// Inherits all display fields from ClipPreview::MidiNote and adds the
// per-note automation event list, which is populated by the editor itself
// from rawMidiData (ClipPreview no longer stores automation events).
struct EditNote : ClipPreview::MidiNote {
    EditNote() = default;
    explicit EditNote(const ClipPreview::MidiNote& base) : ClipPreview::MidiNote(base) {}
    std::vector<ClipPreview::AutomationEvent> automationEvents;
};

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
        // Called on drag-end / velocity edit to commit note changes to the engine.
        // Receives sorted, word-per-entry UMP event + tick arrays.
        std::function<bool(int32_t trackIndex, int32_t clipId,
                           std::vector<uapmd_ump_t> newUmpEvents,
                           std::vector<uint64_t>    newTickTimestamps,
                           std::string&             error)> applyEdits;
        // Called after a successful applyEdits to reload the preview from the engine.
        std::function<std::shared_ptr<ClipPreview>(int32_t trackIndex,
                                                    int32_t clipId)> reloadPreview;
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

    void render(const RenderContext& ctx);

private:
    struct ViewState {
        float hZoom{100.0f};      // Pixels per second at uiScale=1
        float vZoom{12.0f};       // Pixels per note row at uiScale=1
        float vScrollNote{0.0f};  // Vertical scroll in note-slot units (0=top/note127)
        int   snapIdx{3};         // Snap grid: 0=Free 1=1/1 2=1/2 3=1/4 4=1/8 5=1/16 6=1/32
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
    };

    struct WindowState {
        int32_t trackIndex{-1};
        int32_t clipId{-1};
        std::string clipName;
        bool visible{false};
        std::shared_ptr<ClipPreview> preview;
        ViewState view;
        int selectedNoteIdx{-1};
        std::vector<EditNote>                        editNotes;      // mutable working copy of notes
        std::vector<ClipPreview::AutomationEvent>    editClipEvents; // mutable working copy of clip-level automation
        DragState drag;
        bool dirtyAfterEdit{false}; // set when edits should be written back to the engine
        std::vector<size_t> deletedRawIdxs; // rawEventIdx values erased this frame; skipped by applyNoteEdits
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
    void applyNoteEdits(WindowState& state, const RenderContext& ctx);

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
    static void parseAutomationFromRaw(const ClipPreview::RawMidiData& raw,
                                       std::vector<EditNote>& editNotes,
                                       std::vector<ClipPreview::AutomationEvent>& clipEvents);

    // Two-pane NRPN parameter picker popup (no trigger button — caller must call
    // ImGui::OpenPopup(popupId) before this). Left pane shows plugin names; right
    // pane is a scrollable 3-column table. Returns true when a parameter is picked.
    // popupId must be unique per call site (scoped under the current ImGui ID stack).
    static bool renderNrpnPicker(const char* popupId,
                                  uint16_t& paramIndex,
                                  int& hoveredPlugin,
                                  const std::vector<PluginParamEntry>& entries);

    static bool isBlackKey(int midiNote) noexcept;
    static const char* noteNameCStr(int midiNote) noexcept;
    static std::string fullNoteName(int midiNote);
    static const char* automationTypeName(ClipPreview::AutomationEvent::Type t) noexcept;
    static void sortRawMidiEvents(std::vector<uapmd_ump_t>& events,
                                   std::vector<uint64_t>&    ticks);
    static uint64_t secondsToTicks(double seconds, uint32_t tickRes, double bpm) noexcept;
};

} // namespace uapmd::gui
