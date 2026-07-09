#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <imgui.h>
#include <ImTimeline.h>

#include <uapmd-data/uapmd-data.hpp>
#include "ClipPreview.hpp"
#include "TimelineRangeSelection.hpp"
#include "TimelineNavigator.hpp"

namespace uapmd::gui {

// Beats/ticks-based counterpart to SequenceEditor's unified timeline. Renders the same track
// list using the same ImTimeline widget, ClipPreview content, and TrackLegendNodeView legend,
// but positions/sizes clips on a beat axis instead of a seconds axis (see TimelineEditor for the
// per-clip-type position/width formulas that feed this class's ClipRow list). Does not duplicate
// the "Edit Clips..." table window, the audio warp editor, or the piano roll -- those are reached
// via RenderContext::showClipsWindow / showAudioClipEvents / showPianoRoll, which route back to
// the same instances SequenceEditor already owns.
class BeatsSequenceEditor {
public:
    ~BeatsSequenceEditor();

    struct ClipRow {
        int32_t clipId{-1};
        std::string name;           // User-editable clip name
        std::string filepath;       // Full path for waveform/piano roll loading
        bool isMidiClip{false};
        bool isMasterTrack{false};
        int32_t timelineStartTicks{0};  // start position, in kTicksPerBeatDisplay units
        int32_t timelineEndTicks{0};    // end position, in kTicksPerBeatDisplay units
        std::shared_ptr<ClipPreview> customPreview;
    };

    struct RenderContext {
        std::function<void(int32_t trackIndex)> refreshClips;
        std::function<void(int32_t trackIndex, double positionSeconds)> addBlankMidiClipAtPosition;
        std::function<void(int32_t trackIndex, double positionSeconds)> addAudioClip;
        std::function<void(int32_t trackIndex, double positionSeconds)> addSmfClip;
        std::function<void(int32_t trackIndex)> addSmf2Clip;
        std::function<void(int32_t trackIndex, double startSeconds, double endSeconds)> addBlankMidiClipInRange;
        std::function<void(int32_t trackIndex, double startSeconds, double endSeconds)> addEmptyAudioClipInRange;
        std::function<void(int32_t trackIndex, int32_t clipId)> removeClip;
        std::function<void(int32_t trackIndex)> clearAllClips;
        std::function<void(int32_t trackIndex, int32_t clipId, double seconds)> moveClipAbsolute;
        std::function<void(int32_t trackIndex, int32_t clipId)> showMidiClipDump;
        std::function<void(int32_t trackIndex, int32_t clipId)> showAudioClipEvents;
        std::function<void(int32_t trackIndex, int32_t clipId)> showPianoRoll;
        std::function<void()> showMasterTrackDump;
        std::function<void(int32_t trackIndex)> showClipsWindow;  // "Edit Clips..." -> reuses SequenceEditor's window
        std::function<void(int32_t trackIndex, const ImRect& legendArea)> renderLegendContent;
        std::function<double(double seconds)> secondsToBeats;
        std::function<double(double beats)> beatsToSeconds;
        const uapmd::TempoMap* tempoMap = nullptr; // for bar/beat grid math
        const char* timelineUnitsLabel = "beats";
        float uiScale = 1.0f;
        float legendWidth = 0.0f; // required legend pixel width; computed by caller
    };

    void refreshClips(int32_t trackIndex, const std::vector<ClipRow>& clips);
    void removeStaleWindows(int32_t maxValidTrackIndex);
    void invalidateTimeline();
    void reset();

    // Navigation row (zoom slider + position controller); lives in the always-visible toolbar,
    // so it renders separately from (and typically before) renderUnifiedTimeline.
    // barStartScreenX anchors the controller's left edge to the track content column.
    void renderNavigator(const RenderContext& context, float barStartScreenX);
    void renderUnifiedTimeline(const RenderContext& context, float availableHeight);
    float getUnifiedTimelineHeight(float uiScale) const;

    // Zooms so the given content duration (in beats) fits within visibleWidthPixels, clamped to
    // never zoom in past the default. Marks the zoom as user-explicit so the next ordinary
    // rebuild doesn't reset it. No-op if visibleWidthPixels or contentDurationBeats is
    // non-positive.
    void fitToContent(double contentDurationBeats, float visibleWidthPixels, float uiScale);
    // Clip-area width in pixels, cached from the most recent render (0 before any render).
    float lastVisibleWidth() const { return unified_.lastVisibleWidthPixels; }

private:
    struct TrackState {
        TrackState() = default;
        TrackState(const TrackState&) = delete;
        TrackState& operator=(const TrackState&) = delete;
        TrackState(TrackState&&) noexcept = default;
        TrackState& operator=(TrackState&&) noexcept = default;
        ~TrackState() = default;
        std::vector<ClipRow> displayClips;
        int32_t contextMenuClipId = -1;
        double requestedAddPosition = -1.0;
        double requestedRangeStart = -1.0;
        double requestedRangeEnd = -1.0;
        std::unordered_map<int32_t, std::shared_ptr<ClipPreview>> clipPreviews;
    };
    std::unordered_map<int32_t, TrackState> tracks_;

    struct NodeClipRef { int32_t trackIndex; int32_t clipId; };
    struct UnifiedTimelineState {
        std::unique_ptr<ImTimeline::Timeline> timeline;
        ImTimelineStyle style{};
        bool dirty = true;
        std::unordered_map<NodeID, NodeClipRef> nodeToClip;
        NodeID activeDragNodeId = InvalidNodeID;
        std::vector<int32_t> sectionToTrack; // section index -> track index
        float computedTimelineHeight = 0.0f;  // actual height after lane expansion; 0 = use estimate
        RangeSelectionDrag rangeDrag;
        bool hasExplicitZoom = false;  // once true, rebuildUnifiedTimeline stops resetting scale
        float lastVisibleWidthPixels = 0.0f;  // clip-area width, cached from the previous frame
        bool hasPendingFit = false;  // fitToContent was called before any width was known yet
        double pendingFitDurationBeats = 0.0;
        float pendingFitUiScale = 1.0f;
    };
    UnifiedTimelineState unified_;

    void rebuildUnifiedTimeline(const RenderContext& context);
    void drawBeatGridOverlay(
        const RenderContext& context,
        float clipAreaMinX,
        float clipAreaMinY,
        float clipAreaMaxX,
        float clipAreaMaxY
    ) const;
    void drawPlayheadIndicator(
        const RenderContext& context,
        float clipAreaMinX,
        float clipAreaMinY,
        float clipAreaMaxX,
        float clipAreaMaxY
    ) const;
    void drawRangeSelectionOverlay(
        float clipAreaMinX,
        float scale,
        float startFrame,
        float sectionTopY,
        float sectionHeight
    ) const;
    void pruneClipPreviewCache(TrackState& state);
    std::shared_ptr<ClipPreview> ensureClipPreview(int32_t trackIndex, const ClipRow& clip, TrackState& state);
    std::string buildClipSignature(int32_t trackIndex, const ClipRow& clip, const uapmd::ClipData* clipData) const;
    const uapmd::ClipData* findClipData(int32_t trackIndex, int32_t clipId) const;
};

}
