#pragma once

#include <vector>
#include <string>
#include <array>
#include <functional>
#include <imgui.h>
#include "PluginList.hpp"

namespace uapmd::gui {

struct TrackDestinationOption {
    int32_t trackIndex;
    std::string label;
};

class PluginSelector {
public:
    PluginSelector();

    void render();
    void setPlugins(const std::vector<PluginEntry>& plugins);

    // Callbacks
    void setOnInstantiatePlugin(std::function<void(const std::string& format, const std::string& pluginId, int32_t trackIndex)> callback);
    void setOnScanPlugins(std::function<void(bool forceRescan)> callback);

    // State
    void setScanning(bool scanning);
    void setTrackOptions(const std::vector<TrackDestinationOption>& options);
    bool getForceRescan() const { return forceRescan_; }

    // Access to input fields
    const char* getDeviceNameInput() const { return deviceNameInput_; }
    const char* getApiInput() const { return apiInput_; }

private:
    PluginList pluginList_;
    bool forceRescan_ = true;
    bool isScanning_ = false;

    // Track selection for plugin instantiation
    std::vector<TrackDestinationOption> trackOptions_;
    int selectedTrackOption_ = 0; // 0 = new track, 1+ = existing tracks
    char deviceNameInput_[128] = "";  // Empty by default, will use plugin name if not filled
    char apiInput_[64] = "default";

    // Callbacks
    std::function<void(const std::string& format, const std::string& pluginId, int32_t trackIndex)> onInstantiatePlugin_;
    std::function<void(bool forceRescan)> onScanPlugins_;
};

}
