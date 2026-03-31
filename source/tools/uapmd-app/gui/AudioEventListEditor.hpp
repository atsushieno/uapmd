#pragma once

#include <array>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>
#include <uapmd-data/uapmd-data.hpp>

namespace uapmd::gui {

class AudioEventListEditor {
public:
    struct ClipData {
        struct ReferencePointOption {
            std::string label;
            uapmd::AudioWarpReferenceType referenceType{uapmd::AudioWarpReferenceType::Manual};
            std::string referenceClipId;
            std::string referenceMarkerId;
            int64_t clipPositionSamples{0};
            bool resolved{false};
        };

        int32_t trackIndex{-1};
        int32_t clipId{-1};
        std::string clipReferenceId;
        std::string clipName;
        std::string fileLabel;
        std::string filepath;
        int32_t sampleRate{0};
        int64_t durationSamples{0};
        bool markerOnly{false};
        std::vector<uapmd::ClipMarker> markers;
        std::vector<uapmd::AudioWarpPoint> audioWarps;
        std::vector<ReferencePointOption> externalReferenceOptions;
        std::string error;
        bool success{false};
    };

    struct EditPayload {
        int32_t trackIndex{-1};
        int32_t clipId{-1};
        std::vector<uapmd::ClipMarker> markers;
        std::vector<uapmd::AudioWarpPoint> audioWarps;
    };

    struct RenderContext {
        std::function<ClipData(int32_t trackIndex, int32_t clipId)> reloadClip;
        std::function<std::vector<ClipData::ReferencePointOption>(int32_t trackIndex, int32_t clipId)> buildExternalReferenceOptions;
        std::function<bool(int32_t trackIndex, int32_t clipId, const std::string& markerId,
            uapmd::AudioWarpReferenceType referenceType, const std::string& referenceClipId, const std::string& referenceMarkerId)> validateMarkerReference;
        std::function<void(const std::string& windowId, ImVec2 defaultBaseSize)> setNextChildWindowSize;
        std::function<void(const std::string& windowId)> updateChildWindowSizeState;
        std::function<bool(const EditPayload& payload, std::string& error)> applyEdits;
        float uiScale{1.0f};
    };

    void showClip(ClipData data);
    void closeClip(int32_t trackIndex, int32_t clipId);
    void render(const RenderContext& context);
    std::unordered_map<std::string, std::vector<uapmd::ClipMarker>> draftMarkersByClipReference() const;
    std::vector<uapmd::ClipMarker> draftMasterMarkers() const;

    struct MarkerRow {
        std::array<char, 96> markerId{};
        double clipPositionOffset{0.0};
        uapmd::AudioWarpReferenceType referenceType{uapmd::AudioWarpReferenceType::ClipStart};
        std::array<char, 96> referenceClipId{};
        std::array<char, 96> referenceMarkerId{};
        std::array<char, 128> name{};
    };

    struct WarpRow {
        double clipPositionOffset{0.0};
        double speedRatio{1.0};
        uapmd::AudioWarpReferenceType referenceType{uapmd::AudioWarpReferenceType::ClipStart};
        std::array<char, 96> referenceClipId{};
        std::array<char, 96> referenceMarkerId{};
    };

    struct WindowState {
        ClipData data;
        bool visible{true};
        std::string statusMessage;
        ImVec4 statusColor{1.0f, 1.0f, 1.0f, 1.0f};
        std::vector<MarkerRow> markerRows;
        std::vector<WarpRow> warpRows;
    };

private:
    std::map<std::pair<int32_t, int32_t>, WindowState> windows_;

    static std::string buildWindowTitle(const ClipData& data);
    static void populateRows(WindowState& state);
    void renderWindow(WindowState& state, const RenderContext& context);
};

} // namespace uapmd::gui
