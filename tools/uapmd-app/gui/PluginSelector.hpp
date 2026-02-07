#pragma once

#include <vector>
#include <string>
#include <array>
#include <functional>
#include <imgui.h>
#include "PluginList.hpp"

namespace uapmd::gui {

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
    bool getForceRescan() const { return forceRescan_; }
    void setTargetTrackIndex(int32_t trackIndex) { targetTrackIndex_ = trackIndex; }
    int32_t targetTrackIndex() const { return targetTrackIndex_; }

    // Access to input fields
    const char* getDeviceNameInput() const { return deviceNameInput_; }
    const char* getApiInput() const { return apiInput_; }

private:
    PluginList pluginList_;
    bool forceRescan_ = true;
    bool isScanning_ = false;

    // Track selection for plugin instantiation
    int32_t targetTrackIndex_ = -1;
    char deviceNameInput_[128] = "";  // Empty by default, will use plugin name if not filled
    char apiInput_[64] = "default";

    // Callbacks
    std::function<void(const std::string& format, const std::string& pluginId, int32_t trackIndex)> onInstantiatePlugin_;
    std::function<void(bool forceRescan)> onScanPlugins_;
};

}
