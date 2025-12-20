#include "TrackList.hpp"
#include <algorithm>
#include <iostream>
#include <map>

namespace uapmd::gui {

TrackList::TrackList() {
}

void TrackList::setInstances(const std::vector<TrackInstance>& instances) {
    instances_ = instances;
}

void TrackList::render() {
    if (ImGui::Button("Refresh Instances")) {
        if (onRefresh_) {
            onRefresh_();
        }
    }

    ImGui::Text("Active Instances:");

    // Group instances by track
    std::map<int32_t, std::vector<const TrackInstance*>> instancesByTrack;
    std::vector<const TrackInstance*> unassignedInstances;

    for (const auto& instance : instances_) {
        if (instance.trackIndex >= 0) {
            instancesByTrack[instance.trackIndex].push_back(&instance);
        } else {
            unassignedInstances.push_back(&instance);
        }
    }

    if (ImGui::BeginTable("##InstanceTable", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableSetupColumn("Track", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Plugin", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Format", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("UMP Device Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableHeadersRow();

        // Render instances grouped by track
        for (const auto& [trackIndex, trackInstances] : instancesByTrack) {
            for (size_t i = 0; i < trackInstances.size(); i++) {
                renderInstanceRow(*trackInstances[i], i == 0, trackIndex);
            }
        }

        // Render unassigned instances
        for (const auto* instance : unassignedInstances) {
            renderInstanceRow(*instance, true, -1);
        }

        ImGui::EndTable();
    }
}

void TrackList::renderInstanceRow(const TrackInstance& instance, bool showTrackColumn, int32_t trackIndex) {
    ImGui::TableNextRow();

    // Track column
    ImGui::TableSetColumnIndex(0);
    if (showTrackColumn) {
        if (trackIndex >= 0) {
            ImGui::Text("Track %d", trackIndex + 1);
        } else {
            ImGui::TextDisabled("-");
        }
    }

    // Plugin name column
    ImGui::TableSetColumnIndex(1);
    ImGui::Text("%s", instance.pluginName.c_str());

    // Format column
    ImGui::TableSetColumnIndex(2);
    ImGui::Text("%s", instance.pluginFormat.c_str());

    // UMP Device Name column
    ImGui::TableSetColumnIndex(3);
    {
        // Disable textbox if device is running
        if (instance.deviceRunning) {
            ImGui::BeginDisabled();
        }

        std::string inputId = "##ump_name_" + std::to_string(instance.instanceId);
        ImGui::SetNextItemWidth(-FLT_MIN);

        // Create a modifiable copy for ImGui
        static std::map<int32_t, std::array<char, 128>> deviceNameBuffers;
        if (deviceNameBuffers.find(instance.instanceId) == deviceNameBuffers.end()) {
            deviceNameBuffers[instance.instanceId] = {};
            std::strncpy(deviceNameBuffers[instance.instanceId].data(),
                        instance.umpDeviceName.c_str(),
                        deviceNameBuffers[instance.instanceId].size() - 1);
        }

        if (ImGui::InputText(inputId.c_str(), deviceNameBuffers[instance.instanceId].data(),
                            deviceNameBuffers[instance.instanceId].size())) {
            if (onUMPDeviceNameChange_) {
                onUMPDeviceNameChange_(instance.instanceId, std::string(deviceNameBuffers[instance.instanceId].data()));
            }
        }

        if (instance.deviceRunning) {
            ImGui::EndDisabled();
        }
    }

    // Actions column
    ImGui::TableSetColumnIndex(4);
    renderActionsMenu(instance);
}

void TrackList::renderActionsMenu(const TrackInstance& instance) {
    std::string menuButtonId = "Actions##menu" + std::to_string(instance.instanceId);
    std::string popupId = "ActionsPopup##" + std::to_string(instance.instanceId);

    if (ImGui::Button(menuButtonId.c_str())) {
        ImGui::OpenPopup(popupId.c_str());
    }

    if (ImGui::BeginPopup(popupId.c_str())) {
        // Show/Hide UI menu item
        if (!instance.hasUI) {
            ImGui::BeginDisabled();
        }

        const char* uiMenuText = instance.uiVisible ? "Hide UI" : "Show UI";
        if (ImGui::MenuItem(uiMenuText)) {
            if (instance.hasUI) {
                if (instance.uiVisible && onHideUI_) {
                    onHideUI_(instance.instanceId);
                } else if (!instance.uiVisible && onShowUI_) {
                    onShowUI_(instance.instanceId);
                }
            }
        }

        if (!instance.hasUI) {
            ImGui::EndDisabled();
        }

        // Show/Hide Details menu item
        const char* detailsMenuText = "Show Details";  // We don't track details visibility in TrackInstance
        if (ImGui::MenuItem(detailsMenuText)) {
            if (onShowDetails_) {
                onShowDetails_(instance.instanceId);
            }
        }

        // Enable/Disable UMP Device menu items
        if (instance.deviceExists) {
            const char* deviceMenuText = instance.deviceRunning ? "Disable UMP Device" : "Enable UMP Device";

            if (instance.deviceInstantiating) {
                ImGui::BeginDisabled();
            }

            if (ImGui::MenuItem(deviceMenuText)) {
                if (instance.deviceRunning && onDisableDevice_) {
                    onDisableDevice_(instance.instanceId);
                } else if (!instance.deviceRunning && onEnableDevice_) {
                    // Get the current device name from the input buffer
                    static std::map<int32_t, std::array<char, 128>> deviceNameBuffers;
                    std::string deviceName = instance.umpDeviceName;
                    if (deviceNameBuffers.find(instance.instanceId) != deviceNameBuffers.end()) {
                        deviceName = std::string(deviceNameBuffers[instance.instanceId].data());
                    }
                    onEnableDevice_(instance.instanceId, deviceName);
                }
            }

            if (instance.deviceInstantiating) {
                ImGui::EndDisabled();
            }
        }

        ImGui::Separator();

        // Save State menu item
        if (ImGui::MenuItem("Save State")) {
            if (onSaveState_) {
                onSaveState_(instance.instanceId);
            }
        }

        // Load State menu item
        if (ImGui::MenuItem("Load State")) {
            if (onLoadState_) {
                onLoadState_(instance.instanceId);
            }
        }

        ImGui::Separator();

        // Remove menu item
        if (ImGui::MenuItem("Remove")) {
            if (onRemoveInstance_) {
                onRemoveInstance_(instance.instanceId);
            }
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

void TrackList::setOnShowUI(ShowUICallback callback) {
    onShowUI_ = callback;
}

void TrackList::setOnHideUI(HideUICallback callback) {
    onHideUI_ = callback;
}

void TrackList::setOnShowDetails(ShowDetailsCallback callback) {
    onShowDetails_ = callback;
}

void TrackList::setOnHideDetails(HideDetailsCallback callback) {
    onHideDetails_ = callback;
}

void TrackList::setOnEnableDevice(EnableDeviceCallback callback) {
    onEnableDevice_ = callback;
}

void TrackList::setOnDisableDevice(DisableDeviceCallback callback) {
    onDisableDevice_ = callback;
}

void TrackList::setOnSaveState(SaveStateCallback callback) {
    onSaveState_ = callback;
}

void TrackList::setOnLoadState(LoadStateCallback callback) {
    onLoadState_ = callback;
}

void TrackList::setOnRemoveInstance(RemoveInstanceCallback callback) {
    onRemoveInstance_ = callback;
}

void TrackList::setOnRefresh(RefreshCallback callback) {
    onRefresh_ = callback;
}

void TrackList::setOnUMPDeviceNameChange(UMPDeviceNameChangeCallback callback) {
    onUMPDeviceNameChange_ = callback;
}

}
