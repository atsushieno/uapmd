#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ClipPreview.hpp"

namespace uapmd_app_gui {

// A compact MIDI-clip step editor. It writes ordinary MIDI events; the
// Flex-metadata loop convention is kept as a follow-up integration point.
class StepSequencerEditor {
public:
    struct RenderContext {
        float uiScale{1.0f};
        std::function<bool(int32_t, int32_t, std::vector<uapmd_ump_t>,
                           std::vector<uint64_t>, std::string&)> applyEdits;
        std::function<std::shared_ptr<ClipPreview>(int32_t, int32_t)> reloadPreview;
    };

    void showClip(int32_t trackIndex, int32_t clipId, const std::string& clipName,
                  std::shared_ptr<ClipPreview> preview);
    void render(const RenderContext& context);

private:
    struct Step {
        bool active{false};
        float velocity{0.8f};
        float gate{0.8f};
    };

    struct State {
        int32_t trackIndex{-1};
        int32_t clipId{-1};
        std::string clipName;
        bool visible{false};
        std::shared_ptr<ClipPreview> preview;
        uint32_t tickResolution{480};
        uint8_t note{60};
        uint8_t group{0};
        uint8_t channel{0};
        int divisionIndex{3}; // 1/16
        int patternSteps{16};
        int repetitions{1};
        float defaultVelocity{0.8f};
        float defaultGate{0.8f};
        std::vector<Step> steps;
        bool dirty{false};
        std::string status;
    } state_;

    static constexpr int kDivisionCount = 6;
    static constexpr int kDivisions[kDivisionCount] = {1, 2, 4, 8, 16, 32};

    void resetFromPreview();
    void renderWindow(const RenderContext& context);
    bool apply(const RenderContext& context);
    uint64_t stepTicks() const;
    uint64_t patternTicks() const;
};

} // namespace uapmd_app_gui
