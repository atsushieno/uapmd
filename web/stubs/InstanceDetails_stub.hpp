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
        std::function<std::optional<TrackInstance>(int32_t instanceId)> buildTrackInstance;
        std::function<void(int32_t instanceId)> savePluginState;
        std::function<void(int32_t instanceId)> loadPluginState;
        std::function<void(int32_t instanceId)> removeInstance;
        std::function<void(const std::string& windowId, ImVec2 defaultBaseSize)> setNextChildWindowSize;
        std::function<void(const std::string& windowId)> updateChildWindowSizeState;
        float uiScale = 1.0f;
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
