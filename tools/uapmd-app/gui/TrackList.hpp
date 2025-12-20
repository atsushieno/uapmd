#pragma once

#include <vector>
#include <string>
#include <functional>
#include <cstdint>
#include <map>
#include <imgui.h>

namespace uapmd::gui {

struct TrackInstance {
    int32_t instanceId;
    int32_t trackIndex;  // -1 for unassigned
    std::string pluginName;
    std::string pluginFormat;
    std::string umpDeviceName;
    bool hasUI;
    bool uiVisible;
    bool deviceRunning;
    bool deviceExists;
    bool deviceInstantiating;
};

class TrackList {
public:
    using ShowUICallback = std::function<void(int32_t instanceId)>;
    using HideUICallback = std::function<void(int32_t instanceId)>;
    using ShowDetailsCallback = std::function<void(int32_t instanceId)>;
    using HideDetailsCallback = std::function<void(int32_t instanceId)>;
    using EnableDeviceCallback = std::function<void(int32_t instanceId, const std::string& deviceName)>;
    using DisableDeviceCallback = std::function<void(int32_t instanceId)>;
    using SaveStateCallback = std::function<void(int32_t instanceId)>;
    using LoadStateCallback = std::function<void(int32_t instanceId)>;
    using RemoveInstanceCallback = std::function<void(int32_t instanceId)>;
    using RefreshCallback = std::function<void()>;
    using UMPDeviceNameChangeCallback = std::function<void(int32_t instanceId, const std::string& newName)>;

private:
    std::vector<TrackInstance> instances_;

    ShowUICallback onShowUI_;
    HideUICallback onHideUI_;
    ShowDetailsCallback onShowDetails_;
    HideDetailsCallback onHideDetails_;
    EnableDeviceCallback onEnableDevice_;
    DisableDeviceCallback onDisableDevice_;
    SaveStateCallback onSaveState_;
    LoadStateCallback onLoadState_;
    RemoveInstanceCallback onRemoveInstance_;
    RefreshCallback onRefresh_;
    UMPDeviceNameChangeCallback onUMPDeviceNameChange_;

public:
    TrackList();

    void setInstances(const std::vector<TrackInstance>& instances);
    void render();

    void setOnShowUI(ShowUICallback callback);
    void setOnHideUI(HideUICallback callback);
    void setOnShowDetails(ShowDetailsCallback callback);
    void setOnHideDetails(HideDetailsCallback callback);
    void setOnEnableDevice(EnableDeviceCallback callback);
    void setOnDisableDevice(DisableDeviceCallback callback);
    void setOnSaveState(SaveStateCallback callback);
    void setOnLoadState(LoadStateCallback callback);
    void setOnRemoveInstance(RemoveInstanceCallback callback);
    void setOnRefresh(RefreshCallback callback);
    void setOnUMPDeviceNameChange(UMPDeviceNameChangeCallback callback);

private:
    void renderInstanceRow(const TrackInstance& instance, bool showTrackColumn, int32_t trackIndex);
    void renderActionsMenu(const TrackInstance& instance);
};

}
