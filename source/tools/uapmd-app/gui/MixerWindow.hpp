#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <imgui.h>

namespace uapmd {
    class ProjectCommandManager;
    class SequencerEngine;
    class SequencerTrack;
}

namespace uapmd_app_gui {

class MixerWindow {
public:
    struct Callbacks {
        std::function<void(const std::string&, ImVec2)> set_child_size;
        std::function<void(const std::string&)> update_child_size_state;
        std::function<void()> show_latency_monitor;
    };

    void setCallbacks(Callbacks callbacks);
    void toggle();
    void showTrack(int32_t trackIndex);
    void resetMeterDisplay();
    void render(float uiScale);

private:
    struct Meter {
        float level_db{-60.0f};
        float held_peak{};
    };

    struct TrackMeters {
        std::vector<std::vector<Meter>> buses;
        size_t selected_bus{};
    };

    struct Strip;

    Callbacks callbacks_;
    std::unordered_map<std::string, TrackMeters> meters_;
    std::string output_track_;
    std::optional<int32_t> focus_track_;
    std::optional<size_t> reveal_output_;
    bool visible_{};
    bool request_focus_{};
    ImGuiID gesture_id_{};
    uapmd::ProjectCommandManager* gesture_history_{};
    bool gesture_active_{};

    void readMeters(Strip& strip, float releaseDb);
    void renderStrip(uapmd::SequencerEngine& engine, Strip& strip, float meterHeight, float uiScale);
    void renderOutputs(uapmd::SequencerEngine& engine, Strip& strip, float uiScale);
    void updateGesture(uapmd::ProjectCommandManager& history, const char* label);
    void finishGesture(uapmd::ProjectCommandManager* history);
};

}
