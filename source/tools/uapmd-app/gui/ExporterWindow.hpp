#pragma once

#include <array>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

#include <imgui.h>
#include <uapmd-file/IDocumentProvider.hpp>
#include <uapmd-engine/uapmd-engine.hpp>

namespace uapmd::gui {

class ExporterWindow {
public:
    struct Callbacks {
        std::function<void(const std::string&, ImVec2)> setChildSize;
        std::function<void(const std::string&)> updateChildSizeState;
    };

    ExporterWindow() = default;
    explicit ExporterWindow(Callbacks callbacks);

    void setCallbacks(Callbacks callbacks);

    void open();
    void hide();
    bool isVisible() const { return visible_; }

    void render(float uiScale);

private:
    Callbacks callbacks_;
    bool visible_{false};

    struct RenderDialogState {
        enum class RangeMode {
            EntireProject,
            LoopRegion,
            Custom
        };

        RangeMode rangeMode{RangeMode::EntireProject};
        double customStartSeconds{0.0};
        double customEndSeconds{0.0};
        double guardSeconds{2.0};
        bool silenceStopEnabled{true};
        double silenceHoldSeconds{5.0};
        double silenceThresholdDb{-80.0};
        uapmd::OfflineInfiniteTailPolicy infiniteTailPolicy{uapmd::OfflineInfiniteTailPolicy::USE_GUARD_AND_SILENCE_STOP};
        bool hasLoopRange{false};
        double loopStartSeconds{0.0};
        double loopEndSeconds{0.0};
        std::array<char, 512> outputPath{};
        std::optional<uapmd::DocumentHandle> outputHandle;
    };

    RenderDialogState state_{};
    std::filesystem::path lastRenderPath_{};

    void refreshBounds();
    void ensureDefaultPath();
};

} // namespace uapmd::gui
