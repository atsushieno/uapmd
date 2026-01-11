#pragma once

#include <vector>
#include <functional>
#include <optional>
#include <imgui.h>

namespace uapmd::gui {

struct TrackInstance;

class InstanceDetails {
public:
    struct RenderContext {
        float uiScale{1.0f};
        std::function<void(const std::string&, ImVec2)> setNextChildWindowSize;
        std::function<void(const std::string&)> updateChildWindowSizeState;
        std::function<std::optional<TrackInstance>(int32_t)> buildTrackInstance;
    };

    void showWindow(int32_t) {}
    void hideWindow(int32_t) {}
    void removeInstance(int32_t) {}
    void pruneInvalidInstances(const std::vector<int32_t>&) {}
    bool isVisible(int32_t) const { return false; }
    void refreshParametersForInstance(int32_t) {}
    void render(const RenderContext&) {}
};

}

