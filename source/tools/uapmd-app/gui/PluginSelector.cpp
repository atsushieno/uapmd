#include "PluginSelector.hpp"
#include <format>
#include <iostream>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace {
#if defined(__ANDROID__) || defined(__EMSCRIPTEN__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
constexpr bool kRemoteScannerSupported = false;
#else
constexpr bool kRemoteScannerSupported = true;
#endif
}

namespace uapmd::gui {

PluginSelector::PluginSelector() {
}

void PluginSelector::render() {
    if (isScanning_) {
        ImGui::BeginDisabled();
    }

    if (ImGui::Button("Scan Plugins")) {
        if (onScanPlugins_) {
            onScanPlugins_(forceRescan_, useRemoteScanner_);
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

    if constexpr (kRemoteScannerSupported) {
        ImGui::SameLine();
        if (isScanning_)
            ImGui::BeginDisabled();
        ImGui::Checkbox("Remote scanner process", &useRemoteScanner_);
        if (isScanning_)
            ImGui::EndDisabled();
    } else {
        useRemoteScanner_ = false;
    }

    ImGui::Separator();

    // Render the plugin list component
    pluginList_.render();

    // Plugin instantiation controls
    auto selection = pluginList_.getSelection();
    bool canInstantiate = selection.hasSelection;
    if (!canInstantiate) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Instantiate Plugin")) {
        // Call the callback
        if (onInstantiatePlugin_) {
            onInstantiatePlugin_(selection.format, selection.pluginId, targetTrackIndex_);
        }
    }
    if (!canInstantiate) {
        ImGui::EndDisabled();
    }

    if (targetIsMasterTrack_) {
        ImGui::TextUnformatted("Destination: Master Track");
    } else if (targetTrackIndex_ < 0) {
        ImGui::TextUnformatted("Destination: New Track (new UMP device)");
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
    } else {
        ImGui::Text("Destination: Track %d", targetTrackIndex_ + 1);
    }
}

void PluginSelector::setPlugins(const std::vector<PluginEntry>& plugins) {
    pluginList_.setPlugins(plugins);
}

void PluginSelector::setOnInstantiatePlugin(std::function<void(const std::string& format, const std::string& pluginId, int32_t trackIndex)> callback) {
    onInstantiatePlugin_ = callback;
}

void PluginSelector::setOnScanPlugins(std::function<void(bool forceRescan, bool useRemoteProcess)> callback) {
    onScanPlugins_ = callback;
}

void PluginSelector::setScanning(bool scanning) {
    isScanning_ = scanning;
}

void PluginSelector::setTargetTrackIndex(int32_t trackIndex) {
    targetTrackIndex_ = trackIndex;
    targetIsMasterTrack_ = false;
}

void PluginSelector::setTargetNewTrack() {
    targetTrackIndex_ = -1;
    targetIsMasterTrack_ = false;
}

void PluginSelector::setTargetMasterTrack(int32_t masterTrackIndex) {
    targetTrackIndex_ = masterTrackIndex;
    targetIsMasterTrack_ = true;
}

}
