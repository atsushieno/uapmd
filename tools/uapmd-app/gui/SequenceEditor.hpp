#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include <imgui.h>

namespace uapmd::gui {

class SequenceEditor {
public:
    struct ClipRow {
        int32_t clipId{-1};
        int32_t anchorClipId{-1};  // -1 = track anchor
        std::string anchorOrigin;   // "Start" or "End"
        std::string position;       // Display string: "+2.5s"
        std::string name;           // User-editable clip name
        std::string filename;       // Actual file path
        std::string mimeType;
        std::string duration;       // Display string: "5.2s" (only shown when End anchor)
    };

    struct RenderContext {
        std::function<void(int32_t trackIndex)> refreshClips;
        std::function<void(int32_t trackIndex, const std::string& filepath)> addClip;
        std::function<void(int32_t trackIndex, int32_t clipId)> removeClip;
        std::function<void(int32_t trackIndex)> clearAllClips;
        std::function<void(int32_t trackIndex, int32_t clipId, int32_t anchorId, const std::string& origin, const std::string& position)> updateClip;
        std::function<void(int32_t trackIndex, int32_t clipId, const std::string& name)> updateClipName;
        std::function<void(int32_t trackIndex, int32_t clipId)> changeClipFile;
        std::function<void(const std::string& windowId, ImVec2 defaultBaseSize)> setNextChildWindowSize;
        std::function<void(const std::string& windowId)> updateChildWindowSizeState;
        float uiScale = 1.0f;
    };

    void showWindow(int32_t trackIndex);
    void hideWindow(int32_t trackIndex);
    bool isVisible(int32_t trackIndex) const;
    void refreshClips(int32_t trackIndex, const std::vector<ClipRow>& clips);
    void render(const RenderContext& context);

private:
    struct SequenceEditorState {
        bool visible = false;
        std::vector<ClipRow> displayClips;
        int32_t selectedClipId = -1;
    };

    std::unordered_map<int32_t, SequenceEditorState> windows_;

    void renderWindow(int32_t trackIndex, SequenceEditorState& state, const RenderContext& context);
    void renderClipTable(int32_t trackIndex, SequenceEditorState& state, const RenderContext& context);
    void renderClipRow(int32_t trackIndex, const ClipRow& clip, const RenderContext& context);
    std::vector<int32_t> getAnchorOptions(int32_t trackIndex, int32_t currentClipId) const;
};

}
