#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <uapmd/uapmd.hpp>
#include <imgui.h>

#include "MidiKeyboard.hpp"
#include "ParameterList.hpp"
#include "TrackList.hpp"

namespace uapmd::gui {

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

    void showWindow(int32_t instanceId);
    void hideWindow(int32_t instanceId);
    void removeInstance(int32_t instanceId);
    void pruneInvalidInstances(const std::vector<int32_t>& validInstanceIds);
    bool isVisible(int32_t instanceId) const;
    void render(const RenderContext& context);
    void refreshParametersForInstance(int32_t instanceId);

private:
    struct DetailsWindowState {
        MidiKeyboard midiKeyboard;
        ParameterList parameterList;
        bool visible = false;
        std::vector<uapmd::PresetsMetadata> presets;
        int selectedPreset = -1;
        float pitchBendValue = 0.0f; // -1..1 UI range
        float channelPressureValue = 0.0f; // 0..1 UI range
    };

    std::unordered_map<int32_t, DetailsWindowState> windows_;

    void refreshParameters(int32_t instanceId, DetailsWindowState& state);
    void refreshPresets(int32_t instanceId, DetailsWindowState& state);
    void loadSelectedPreset(int32_t instanceId, DetailsWindowState& state);
    void applyParameterUpdates(int32_t instanceId, DetailsWindowState& state);
    void renderParameterControls(int32_t instanceId, DetailsWindowState& state);
};

}
