#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <imgui.h>
#include <memory>
#include <ImTimeline.h>

namespace uapmd::gui {

class SequenceEditor {
public:
    ~SequenceEditor();

    struct ClipRow {
        int32_t clipId{-1};
        int32_t anchorClipId{-1};  // -1 = track anchor
        std::string anchorOrigin;   // "Start" or "End"
        std::string position;       // Display string: "+2.5s"
        std::string name;           // User-editable clip name
        std::string filename;       // Actual file path
        std::string mimeType;
        std::string duration;       // Display string: "5.2s" (only shown when End anchor)
        int32_t timelineStart = 0;   // Timeline start in milliseconds
        int32_t timelineEnd = 0;     // Timeline end in milliseconds
    };

    struct RenderContext {
        std::function<void(int32_t trackIndex)> refreshClips;
        std::function<void(int32_t trackIndex, const std::string& filepath)> addClip;
        std::function<void(int32_t trackIndex, int32_t clipId)> removeClip;
        std::function<void(int32_t trackIndex)> clearAllClips;
        std::function<void(int32_t trackIndex, int32_t clipId, int32_t anchorId, const std::string& origin, const std::string& position)> updateClip;
        std::function<void(int32_t trackIndex, int32_t clipId, const std::string& name)> updateClipName;
        std::function<void(int32_t trackIndex, int32_t clipId)> changeClipFile;
        std::function<void(int32_t trackIndex, int32_t clipId, double seconds)> moveClipAbsolute;
        std::function<void(const std::string& windowId, ImVec2 defaultBaseSize)> setNextChildWindowSize;
        std::function<void(const std::string& windowId)> updateChildWindowSizeState;
        float uiScale = 1.0f;
    };

    void showWindow(int32_t trackIndex);
    void hideWindow(int32_t trackIndex);
    bool isVisible(int32_t trackIndex) const;
    void refreshClips(int32_t trackIndex, const std::vector<ClipRow>& clips);
    void render(const RenderContext& context);
    void removeStaleWindows(int32_t maxValidTrackIndex);
    void renderTimelineInline(int32_t trackIndex, const RenderContext& context, float availableHeight);
    void reset();
    float getInlineTimelineHeight(int32_t trackIndex, float uiScale) const;

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
        std::unique_ptr<ImTimeline::Timeline> timeline;
        ImTimelineStyle timelineStyle{};
        bool timelineDirty = true;
        std::unordered_map<int32_t, int32_t> sectionToClip;
        std::unordered_map<int32_t, uint64_t> sectionToNodeId;  // Maps section ID to NodeID
        int32_t activeDragSection = -1;
    };

    std::unordered_map<int32_t, SequenceEditorState> windows_;

    void renderWindow(int32_t trackIndex, SequenceEditorState& state, const RenderContext& context);
    void renderClipTable(int32_t trackIndex, SequenceEditorState& state, const RenderContext& context, float availableHeight);
    void renderTimelineContent(int32_t trackIndex, SequenceEditorState& state, const RenderContext& context, float availableHeight, bool showLabel);
    void rebuildTimelineModel(int32_t trackIndex, SequenceEditorState& state, const RenderContext& context);
    void renderClipRow(int32_t trackIndex, const ClipRow& clip, const RenderContext& context);
    bool renderAnchorCombo(int32_t trackIndex, const ClipRow& clip, const RenderContext& context);
    bool renderOriginCombo(int32_t trackIndex, const ClipRow& clip, const RenderContext& context);
    bool renderPositionInput(int32_t trackIndex, const ClipRow& clip, const RenderContext& context);
    bool renderNameInput(int32_t trackIndex, const ClipRow& clip, const RenderContext& context);
    std::vector<int32_t> getAnchorOptions(int32_t trackIndex, int32_t currentClipId) const;
};

}
