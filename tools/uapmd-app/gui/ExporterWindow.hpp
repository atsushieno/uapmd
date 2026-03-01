#pragma once

#include <array>
#include <filesystem>
#include <functional>
#include <string>

#include <imgui.h>

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
        bool hasLoopRange{false};
        double loopStartSeconds{0.0};
        double loopEndSeconds{0.0};
        std::array<char, 512> outputPath{};
    };

    RenderDialogState state_{};
    std::filesystem::path lastRenderPath_{};

    void refreshBounds();
    void ensureDefaultPath();
};

} // namespace uapmd::gui

