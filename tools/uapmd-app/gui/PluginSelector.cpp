#include "PluginSelector.hpp"
#include <format>
#include <iostream>

namespace uapmd::gui {

PluginSelector::PluginSelector() {
}

void PluginSelector::render() {
    if (isScanning_) {
        ImGui::BeginDisabled();
    }

    if (ImGui::Button("Scan Plugins")) {
        if (onScanPlugins_) {
            onScanPlugins_(forceRescan_);
        }
        std::cout << "Starting plugin scanning" << std::endl;
    }

    if (isScanning_) {
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::Text("Scanning...");
    } else {
        ImGui::SameLine();
        ImGui::Checkbox("Force Rescan", &forceRescan_);
    }

    ImGui::Separator();

    // Render the plugin list component
    pluginList_.render();

    ImGui::Separator();
    // Build track destination options
    std::vector<std::string> labels;
    {
        if (selectedTrackOption_ < 0 || selectedTrackOption_ > static_cast<int>(trackOptions_.size())) {
            selectedTrackOption_ = 0;
        }

        labels.reserve(trackOptions_.size() + 1);
        labels.emplace_back("New track (new UMP device)");
        for (const auto& option : trackOptions_) {
            labels.push_back(option.label);
        }
    }

    std::vector<const char*> labelPtrs;
    labelPtrs.reserve(labels.size());
    for (auto& label : labels) {
        labelPtrs.push_back(label.c_str());
    }

    // Plugin instantiation controls
    auto selection = pluginList_.getSelection();
    bool canInstantiate = selection.hasSelection;
    if (!canInstantiate) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Instantiate Plugin")) {
        // Determine track index
        int32_t trackIndex = -1;
        if (selectedTrackOption_ > 0 && static_cast<size_t>(selectedTrackOption_ - 1) < trackOptions_.size()) {
            // Use existing track
            trackIndex = trackOptions_[static_cast<size_t>(selectedTrackOption_ - 1)].trackIndex;
        }

        // Call the callback
        if (onInstantiatePlugin_) {
            onInstantiatePlugin_(selection.format, selection.pluginId, trackIndex);
        }
    }
    if (!canInstantiate) {
        ImGui::EndDisabled();
    }

    ImGui::SameLine();
    ImGui::TextUnformatted("on");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(250.0f);
    ImGui::Combo("##track_dest", &selectedTrackOption_, labelPtrs.data(), static_cast<int>(labelPtrs.size()));

    if (selectedTrackOption_ == 0) {
        // Show device configuration for new track
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Device Name:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(200.0f);
        ImGui::InputText("##device_name", deviceNameInput_, sizeof(deviceNameInput_));
        ImGui::SameLine();
        ImGui::TextUnformatted("API:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        ImGui::InputText("##api", apiInput_, sizeof(apiInput_));
    }
}

void PluginSelector::setPlugins(const std::vector<PluginEntry>& plugins) {
    pluginList_.setPlugins(plugins);
}

void PluginSelector::setOnInstantiatePlugin(std::function<void(const std::string& format, const std::string& pluginId, int32_t trackIndex)> callback) {
    onInstantiatePlugin_ = callback;
}

void PluginSelector::setOnScanPlugins(std::function<void(bool forceRescan)> callback) {
    onScanPlugins_ = callback;
}

void PluginSelector::setScanning(bool scanning) {
    isScanning_ = scanning;
}

void PluginSelector::setTrackOptions(const std::vector<TrackDestinationOption>& options) {
    trackOptions_ = options;
}

}
