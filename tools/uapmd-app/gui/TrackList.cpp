#include "TrackList.hpp"
#include <algorithm>
#include <array>
#include <iostream>
#include <map>

namespace uapmd::gui {

TrackList::TrackList() {
}

void TrackList::setInstances(const std::vector<TrackInstance>& instances) {
    instances_ = instances;
}

void TrackList::render() {
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
        ImGui::TableSetupColumn("UMP Device", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Details", ImGuiTableColumnFlags_WidthFixed, 80.0f);
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

    // UMP Device column
    ImGui::TableSetColumnIndex(3);
    {
        bool disableDeviceControls = instance.deviceInstantiating;
        if (disableDeviceControls) {
            ImGui::BeginDisabled();
        }

        std::string inputId = "##ump_name_" + std::to_string(instance.instanceId);
        float availableWidth = ImGui::GetContentRegionAvail().x;
        float buttonWidth = 90.0f;
        float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
        ImGui::SetNextItemWidth(std::max(0.0f, availableWidth - (buttonWidth + spacing)));

        // Create a modifiable copy for ImGui
        static std::map<int32_t, std::array<char, 128>> deviceNameBuffers;
        if (deviceNameBuffers.find(instance.instanceId) == deviceNameBuffers.end()) {
            deviceNameBuffers[instance.instanceId] = {};
            strncpy(deviceNameBuffers[instance.instanceId].data(),
                        instance.umpDeviceName.c_str(),
                        deviceNameBuffers[instance.instanceId].size() - 1);
        }

        bool disableNameEdit = instance.deviceRunning;
        if (disableNameEdit) {
            ImGui::BeginDisabled();
        }

        if (ImGui::InputText(inputId.c_str(), deviceNameBuffers[instance.instanceId].data(),
                            deviceNameBuffers[instance.instanceId].size())) {
            if (onUMPDeviceNameChange_) {
                onUMPDeviceNameChange_(instance.instanceId, std::string(deviceNameBuffers[instance.instanceId].data()));
            }
        }

        if (disableNameEdit) {
            ImGui::EndDisabled();
        }

        ImGui::SameLine();
        std::string deviceButtonLabel = (instance.deviceRunning ? "Disable##" : "Enable##") + std::to_string(instance.instanceId);
        if (ImGui::Button(deviceButtonLabel.c_str(), ImVec2(buttonWidth, 0.0f))) {
            if (instance.deviceRunning) {
                if (onDisableDevice_) {
                    onDisableDevice_(instance.instanceId);
                }
            } else if (onEnableDevice_) {
                // Get the current device name from the input buffer
                std::string deviceName = instance.umpDeviceName;
                if (deviceNameBuffers.find(instance.instanceId) != deviceNameBuffers.end()) {
                    deviceName = std::string(deviceNameBuffers[instance.instanceId].data());
                }
                onEnableDevice_(instance.instanceId, deviceName);
            }
        }

        if (disableDeviceControls) {
            ImGui::EndDisabled();
        }
    }

    // Details toggle column
    ImGui::TableSetColumnIndex(4);
    std::string buttonLabel = (instance.detailsVisible ? "Hide##" : "Show##") + std::to_string(instance.instanceId);
    if (ImGui::Button(buttonLabel.c_str())) {
        if (instance.detailsVisible) {
            if (onHideDetails_) {
                onHideDetails_(instance.instanceId);
            }
        } else if (onShowDetails_) {
            onShowDetails_(instance.instanceId);
        }
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

void TrackList::setOnUMPDeviceNameChange(UMPDeviceNameChangeCallback callback) {
    onUMPDeviceNameChange_ = callback;
}

}
