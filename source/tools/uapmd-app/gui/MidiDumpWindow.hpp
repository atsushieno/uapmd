#pragma once

#include <array>
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
        uint64_t deltaTicks{0};
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
        bool isMasterTrack{false};
    };

    struct ParsedEvent {
        uint64_t tickPosition{0};
        std::vector<uint32_t> words;
    };

    struct EditPayload {
        int32_t trackIndex{-1};
        int32_t clipId{-1};
        uint32_t tickResolution{0};
        bool isMasterTrack{false};
        std::vector<ParsedEvent> events;
    };

    struct RenderContext {
        std::function<ClipDumpData(int32_t trackIndex, int32_t clipId)> reloadClip;
        std::function<void(const std::string& windowId, ImVec2 defaultBaseSize)> setNextChildWindowSize;
        std::function<void(const std::string& windowId)> updateChildWindowSizeState;
        std::function<bool(const EditPayload& payload, std::string& error)> applyEdits;
        float uiScale{1.0f};
    };

    void showClipDump(ClipDumpData data);
    void closeClip(int32_t trackIndex, int32_t clipId);
    void render(const RenderContext& context);

        struct WindowState {
            ClipDumpData data;
            bool visible{true};
            bool readOnly{false};
            bool hasPendingChanges{false};
        std::string statusMessage;
        ImVec4 statusColor{1.0f, 1.0f, 1.0f, 1.0f};

            struct EditableRow {
                uint64_t tickPosition{0};
                double cachedSeconds{0.0};
                std::array<char, 64> tickBuffer{};
                std::array<char, 64> lengthBuffer{};
                std::array<char, 256> hexBuffer{};
                std::array<char, 64> lengthEditBuffer{};
                std::array<char, 256> hexEditBuffer{};
                std::vector<uint32_t> parsedWords;
                bool lengthValid{true};
                bool hexValid{true};
                std::string lengthError;
                std::string hexError;
                bool editingLength{false};
                bool editingHex{false};
                bool focusLength{false};
                bool focusHex{false};
            };

        std::vector<EditableRow> rows;
    };

private:
    std::map<std::pair<int32_t, int32_t>, WindowState> windows_;

    static std::string buildWindowTitle(const ClipDumpData& data);
    void renderWindow(WindowState& state, const RenderContext& context);
};

} // namespace uapmd::gui
