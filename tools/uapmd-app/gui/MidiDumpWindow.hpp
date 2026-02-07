#pragma once

#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>

namespace uapmd::gui {

class MidiDumpWindow {
public:
    struct EventRow {
        double timeSeconds{0.0};
        uint64_t tickPosition{0};
        size_t lengthBytes{0};
        std::string timeLabel;
        std::string hexBytes;
    };

    struct ClipDumpData {
        int32_t trackIndex{-1};
        int32_t clipId{-1};
        std::string clipName;
        std::string fileLabel;
        std::string filepath;
        uint32_t tickResolution{0};
        double tempo{0.0};
        std::vector<EventRow> events;
        std::string error;
        bool success{false};
    };

    struct RenderContext {
        std::function<ClipDumpData(int32_t trackIndex, int32_t clipId)> reloadClip;
        std::function<void(const std::string& windowId, ImVec2 defaultBaseSize)> setNextChildWindowSize;
        std::function<void(const std::string& windowId)> updateChildWindowSizeState;
        float uiScale{1.0f};
    };

    void showClipDump(ClipDumpData data);
    void closeClip(int32_t trackIndex, int32_t clipId);
    void render(const RenderContext& context);

private:
    struct WindowState {
        ClipDumpData data;
        bool visible{true};
    };

    std::map<std::pair<int32_t, int32_t>, WindowState> windows_;

    static std::string buildWindowTitle(const ClipDumpData& data);
    void renderWindow(WindowState& state, const RenderContext& context);
};

} // namespace uapmd::gui
