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

namespace uapmd::gui {

class SequenceEditor {
public:
    ~SequenceEditor();

    struct ClipRow {
        int32_t clipId{-1};
        std::string referenceId;
        std::string trackReferenceId;
        std::string anchorReferenceId;
        std::string anchorOrigin;   // "Start" or "End"
        std::string position;       // Display string: "+2.5s"
        std::string name;           // User-editable clip name
        std::string filename;       // Actual file path
        std::string filepath;       // Full path for waveform/piano roll loading
        std::string mimeType;
        std::string duration;       // Display string: "5.2s" (only shown when End anchor)
        int32_t timelineStart = 0;   // Timeline start in milliseconds
        int32_t timelineEnd = 0;     // Timeline end in milliseconds
        bool isMidiClip = false;
        bool isMasterTrack = false;
        std::shared_ptr<ClipPreview> customPreview;
    };

    struct RenderContext {
        std::function<void(int32_t trackIndex)> refreshClips;
        std::function<void(int32_t trackIndex, const std::string& filepath)> addClip;
        std::function<void(int32_t trackIndex, const std::string& filepath, double positionSeconds)> addClipAtPosition;
        std::function<void(int32_t trackIndex)> addAudioClip;
        std::function<void(int32_t trackIndex)> addSmfClip;
        std::function<void(int32_t trackIndex)> addSmf2Clip;
        std::function<void(int32_t trackIndex, double positionSeconds)> addBlankMidiClipAtPosition;
        std::function<void(int32_t trackIndex, int32_t clipId)> removeClip;
        std::function<void(int32_t trackIndex)> clearAllClips;
        std::function<void(int32_t trackIndex, int32_t clipId, const std::string& anchorReferenceId, const std::string& origin, const std::string& position)> updateClip;
        std::function<void(int32_t trackIndex, int32_t clipId, const std::string& name)> updateClipName;
        std::function<void(int32_t trackIndex, int32_t clipId)> changeClipFile;
        std::function<void(int32_t trackIndex, int32_t clipId, double seconds)> moveClipAbsolute;
        std::function<void(int32_t trackIndex, int32_t clipId)> showMidiClipDump;
        std::function<void(int32_t trackIndex, int32_t clipId)> showAudioClipEvents;
        std::function<void(int32_t trackIndex, int32_t clipId)> showPianoRoll;
        std::function<void()> showMasterTrackDump;
        std::function<void(const std::string& windowId, ImVec2 defaultBaseSize)> setNextChildWindowSize;
        std::function<void(const std::string& windowId)> updateChildWindowSizeState;
        std::function<double(double seconds)> secondsToTimelineUnits;
        std::function<double(double units)> timelineUnitsToSeconds;
        std::function<void(int32_t trackIndex, const ImRect& legendArea)> renderLegendContent;
        const char* timelineUnitsLabel = "seconds";
        float uiScale = 1.0f;
        float legendWidth = 0.0f; // required legend pixel width; computed by caller
    };

    void showWindow(int32_t trackIndex);
    void hideWindow(int32_t trackIndex);
    bool isVisible(int32_t trackIndex) const;
    void refreshClips(int32_t trackIndex, const std::vector<ClipRow>& clips);
    void render(const RenderContext& context);
    void removeStaleWindows(int32_t maxValidTrackIndex);
    void reset();

    void renderUnifiedTimeline(const RenderContext& context, float availableHeight);
    float getUnifiedTimelineHeight(float uiScale) const;

private:
    struct SequenceEditorState {
        SequenceEditorState() = default;
        SequenceEditorState(const SequenceEditorState&) = delete;
        SequenceEditorState& operator=(const SequenceEditorState&) = delete;
        SequenceEditorState(SequenceEditorState&&) noexcept = default;
        SequenceEditorState& operator=(SequenceEditorState&&) noexcept = default;
        ~SequenceEditorState() = default;
        bool visible = false;
        std::vector<ClipRow> displayClips;
        int32_t selectedClipId = -1;
        int32_t contextMenuClipId = -1;
        double requestedAddPosition = -1.0;
        std::unordered_map<int32_t, std::shared_ptr<ClipPreview>> clipPreviews;
    };
    std::unordered_map<int32_t, SequenceEditorState> windows_;

    struct NodeClipRef { int32_t trackIndex; int32_t clipId; };
    struct UnifiedTimelineState {
        std::unique_ptr<ImTimeline::Timeline> timeline;
        ImTimelineStyle style{};
        bool dirty = true;
        std::unordered_map<NodeID, NodeClipRef> nodeToClip;
        NodeID activeDragNodeId = InvalidNodeID;
        std::vector<int32_t> sectionToTrack; // section index -> track index
    };
    UnifiedTimelineState unified_;

    void renderWindow(int32_t trackIndex, SequenceEditorState& state, const RenderContext& context);
    void renderClipTable(int32_t trackIndex, SequenceEditorState& state, const RenderContext& context, float availableHeight);
    void rebuildUnifiedTimeline(const RenderContext& context);
    void renderClipRow(int32_t trackIndex, const ClipRow& clip, const RenderContext& context);
    bool renderAnchorCombo(int32_t trackIndex, const ClipRow& clip, const RenderContext& context);
    bool renderOriginCombo(int32_t trackIndex, const ClipRow& clip, const RenderContext& context);
    bool renderPositionInput(int32_t trackIndex, const ClipRow& clip, const RenderContext& context);
    bool renderNameInput(int32_t trackIndex, const ClipRow& clip, const RenderContext& context);
    struct AnchorOption {
        int32_t trackIndex{-1};
        std::string clipReferenceId;
        std::string label;
    };

    std::vector<AnchorOption> getAnchorOptions(int32_t trackIndex, int32_t currentClipId) const;
    void drawPlayheadIndicator(
        const RenderContext& context,
        float clipAreaMinX,
        float clipAreaMinY,
        float clipAreaMaxX,
        float clipAreaMaxY
    ) const;
    void pruneClipPreviewCache(SequenceEditorState& state);
    std::shared_ptr<ClipPreview> ensureClipPreview(int32_t trackIndex, const ClipRow& clip, SequenceEditorState& state);
    std::string buildClipSignature(int32_t trackIndex, const ClipRow& clip, const uapmd::ClipData* clipData) const;
    const uapmd::ClipData* findClipData(int32_t trackIndex, int32_t clipId) const;
};

}
