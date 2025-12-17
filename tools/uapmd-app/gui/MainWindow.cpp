#include <cctype>
#include <cstring>
#include <algorithm>
#include <map>
#include <format>
#include <iostream>
#include <fstream>
#include <optional>
#include <ranges>
#include <unordered_map>
#include <unordered_set>
#include <portable-file-dialogs.h>
#include <choc/audio/choc_AudioFileFormat_WAV.h>
#include <choc/audio/choc_AudioFileFormat_FLAC.h>
#include <choc/audio/choc_AudioFileFormat_Ogg.h>

#include <midicci/midicci.hpp> // include before anything that indirectly includes X.h

#include <imgui.h>

#include "SharedTheme.hpp"

#include "MainWindow.hpp"
#include "../AppModel.hpp"

namespace {
std::string formatPlainValueLabel(double value) {
    return std::format("{:.7g}", value);
}
}

namespace uapmd::gui {
MainWindow::MainWindow(GuiDefaults defaults) {
    SetupImGuiStyle();

    // Apply defaults from command line arguments
    if (!defaults.pluginName.empty()) {
        selectedPluginFormat_ = defaults.formatName;
        // Plugin selection will be applied after plugin list is loaded
    }

    // Initialize with some example recent files
    recentFiles_.push_back("example1.wav");
    recentFiles_.push_back("example2.mid");
    recentFiles_.push_back("example3.wav");

    refreshDeviceList();
    refreshInstances();
    refreshPluginList();

    // Register callback for when plugin instantiation completes
    uapmd::AppModel::instance().instancingCompleted.push_back(
        [this](int32_t instancingId, int32_t instanceId, std::string error) {
            if (error.empty()) {
                // Instantiation successful, refresh the instance list
                refreshInstances();
            }
        });

    // Register callback for when plugin scanning completes
    uapmd::AppModel::instance().scanningCompleted.push_back(
        [this](bool success, std::string error) {
            if (success) {
                // Scanning successful, refresh the plugin list
                refreshPluginList();
                std::cout << "Plugin list refreshed after scanning" << std::endl;
            } else {
                std::cout << "Plugin scanning failed: " << error << std::endl;
            }
        });
}

void MainWindow::render(void* window) {
    // Use the entire screen space as the main window (no nested window)
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 8.0f));

    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

    if (ImGui::Begin("MainAppWindow", nullptr, window_flags)) {
        // Device Settings Section
        if (ImGui::CollapsingHeader("Device Settings", ImGuiTreeNodeFlags_None)) {
            renderDeviceSettings();
        }

        ImGui::Separator();

        // Player Settings Section
        if (ImGui::CollapsingHeader("Player Settings", ImGuiTreeNodeFlags_None)) {
            renderPlayerSettings();
        }

        ImGui::Separator();

        // Select a Plugin Button - positioned before instance control for better visibility
        if (ImGui::Button(showPluginSelector_ ? "Hide Plugin List" : "Select a Plugin", ImVec2(200, 40))) {
            showPluginSelector_ = !showPluginSelector_;
        }

        if (showPluginSelector_) {
            renderPluginSelector();
        }

        ImGui::Separator();

        // Instance Control Section
        if (ImGui::CollapsingHeader("Instance Control", ImGuiTreeNodeFlags_DefaultOpen)) {
            renderInstanceControl();
        }

        ImGui::Separator();

        // Virtual MIDI Device controls are now in the Actions menu of Instance Control
    }
    ImGui::End();
    ImGui::PopStyleVar(3);

    // Render details windows as separate ImGui floating windows
    renderDetailsWindows();
}

void MainWindow::update() {
}

bool MainWindow::handlePluginResizeRequest(int32_t instanceId, uint32_t width, uint32_t height) {
    auto windowIt = pluginWindows_.find(instanceId);
    if (windowIt == pluginWindows_.end())
        return false;

    auto* window = windowIt->second.get();
    if (!window)
        return false;

    auto& bounds = pluginWindowBounds_[instanceId];
    remidy::gui::Bounds currentBounds = bounds;
    // Keep existing x/y from stored bounds; containers report size via our own state
    bounds.width = static_cast<int>(width);
    bounds.height = static_cast<int>(height);

    pluginWindowResizeIgnore_.insert(instanceId);

    auto& sequencer = uapmd::AppModel::instance().sequencer();
    bool success = true;
    remidy::EventLoop::runTaskOnMainThread([window, &bounds, &success]() {
        if (!window) {
            success = false;
            return;
        }
        window->resize(bounds.width, bounds.height);
    });

    if (!success)
        pluginWindowResizeIgnore_.erase(instanceId);

    auto* instance = sequencer.getPluginInstance(instanceId);
    if (!instance->setUISize(width, height)) {
        uint32_t adjustedWidth = width;
        uint32_t adjustedHeight = height;
        if (instance->getUISize(adjustedWidth, adjustedHeight)) {
            pluginWindowBounds_[instanceId].width = static_cast<int>(adjustedWidth);
            pluginWindowBounds_[instanceId].height = static_cast<int>(adjustedHeight);
            pluginWindowResizeIgnore_.insert(instanceId);
            remidy::EventLoop::runTaskOnMainThread([window, w = static_cast<int>(adjustedWidth), h = static_cast<int>(adjustedHeight)]() {
                if (!window)
                    return;
                window->resize(w, h);
            });
        }
    }

    return success;
}

void MainWindow::onPluginWindowResized(int32_t instanceId) {
    auto windowIt = pluginWindows_.find(instanceId);
    if (windowIt == pluginWindows_.end())
        return;

    if (pluginWindowResizeIgnore_.erase(instanceId) > 0)
        return;

    auto* window = windowIt->second.get();
    if (!window)
        return;

    remidy::gui::Bounds currentBounds = pluginWindowBounds_[instanceId];

    auto& sequencer = uapmd::AppModel::instance().sequencer();
    pluginWindowBounds_[instanceId] = currentBounds;

    const uint32_t currentWidth = static_cast<uint32_t>(std::max(currentBounds.width, 0));
    const uint32_t currentHeight = static_cast<uint32_t>(std::max(currentBounds.height, 0));

    auto* instance = sequencer.getPluginInstance(instanceId);
    if (instance->setUISize(currentWidth, currentHeight))
        return;

    uint32_t adjustedWidth = currentWidth;
    uint32_t adjustedHeight = currentHeight;
    if (!instance->getUISize(adjustedWidth, adjustedHeight))
        return;

    if (adjustedWidth == currentWidth && adjustedHeight == currentHeight)
        return;

    pluginWindowBounds_[instanceId].width = static_cast<int>(adjustedWidth);
    pluginWindowBounds_[instanceId].height = static_cast<int>(adjustedHeight);
    pluginWindowResizeIgnore_.insert(instanceId);

    remidy::EventLoop::runTaskOnMainThread([window, w = static_cast<int>(adjustedWidth), h = static_cast<int>(adjustedHeight)]() {
        if (!window)
            return;
        window->resize(w, h);
    });
}

void MainWindow::onPluginWindowClosed(int32_t instanceId) {
    auto& sequencer = uapmd::AppModel::instance().sequencer();
    // Just update the visible flag in the plugin - don't touch the window or embedded state
    sequencer.getPluginInstance(instanceId)->hideUI();
}

bool MainWindow::fetchPluginUISize(int32_t instanceId, uint32_t &width, uint32_t &height) {
    auto& sequencer = uapmd::AppModel::instance().sequencer();
    auto* instance = sequencer.getPluginInstance(instanceId);
    if (!instance->hasUISupport())
        return false;
    return instance->getUISize(width, height);
}

void MainWindow::renderDeviceSettings() {
    ImGui::Text("Audio Device Configuration:");

    // Input device selection
    if (ImGui::BeginCombo("Input Device", selectedInputDevice_ < inputDevices_.size() ? inputDevices_[selectedInputDevice_].c_str() : "None")) {
        for (size_t i = 0; i < inputDevices_.size(); i++) {
            bool isSelected = (selectedInputDevice_ == static_cast<int>(i));
            if (ImGui::Selectable(inputDevices_[i].c_str(), isSelected)) {
                selectedInputDevice_ = static_cast<int>(i);
            }
            if (isSelected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    // Output device selection
    if (ImGui::BeginCombo("Output Device", selectedOutputDevice_ < outputDevices_.size() ? outputDevices_[selectedOutputDevice_].c_str() : "None")) {
        for (size_t i = 0; i < outputDevices_.size(); i++) {
            bool isSelected = (selectedOutputDevice_ == static_cast<int>(i));
            if (ImGui::Selectable(outputDevices_[i].c_str(), isSelected)) {
                selectedOutputDevice_ = static_cast<int>(i);
            }
            if (isSelected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    // Buffer size and sample rate
    ImGui::InputInt("Buffer Size", &bufferSize_);
    ImGui::InputInt("Sample Rate", &sampleRate_);

    // Apply and refresh buttons
    if (ImGui::Button("Apply Settings")) {
        applyDeviceSettings();
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh Devices")) {
        refreshDeviceList();
    }
}

void MainWindow::renderPlayerSettings() {
    auto& sequencer = uapmd::AppModel::instance().sequencer();

    ImGui::Text("Current File: %s", currentFile_.empty() ? "None" : currentFile_.c_str());

    if (ImGui::Button("Load File...")) {
        loadFile();
    }

    // Spectrum analyzers - side by side
    // Update spectrum data from sequencer
    sequencer.getInputSpectrum(inputSpectrum_, kSpectrumBars);
    sequencer.getOutputSpectrum(outputSpectrum_, kSpectrumBars);

    float availableWidth = ImGui::GetContentRegionAvail().x;
    float spectrumWidth = (availableWidth - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    ImVec2 spectrumSize = ImVec2(spectrumWidth, 64);

    // Use table layout for proper alignment
    if (ImGui::BeginTable("##SpectrumTable", 2, ImGuiTableFlags_None))
    {
        ImGui::TableSetupColumn("Input", ImGuiTableColumnFlags_WidthFixed, spectrumWidth);
        ImGui::TableSetupColumn("Output", ImGuiTableColumnFlags_WidthFixed, spectrumWidth);

        // First row: labels
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Text("Input");
        ImGui::TableNextColumn();
        ImGui::Text("Output");

        // Second row: histograms
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::PlotHistogram("##InputSpectrum", inputSpectrum_, kSpectrumBars, 0, nullptr, 0.0f, 1.0f, spectrumSize);
        ImGui::TableNextColumn();
        ImGui::PlotHistogram("##OutputSpectrum", outputSpectrum_, kSpectrumBars, 0, nullptr, 0.0f, 1.0f, spectrumSize);

        ImGui::EndTable();
    }

    ImGui::Text("Transport Controls:");

    // Play/Stop button
    const char* playStopButtonText = isPlaying_ ? "Stop" : "Play";
    if (ImGui::Button(playStopButtonText)) {
        if (isPlaying_) {
            stop();
        } else {
            play();
        }
    }

    ImGui::SameLine();

    // Pause/Resume button - only enabled when playing
    if (!isPlaying_) {
        ImGui::BeginDisabled();
    }
    const char* pauseResumeButtonText = isPaused_ ? "Resume" : "Pause";
    if (ImGui::Button(pauseResumeButtonText)) {
        if (isPaused_) {
            resume();
        } else {
            pause();
        }
    }
    if (!isPlaying_) {
        ImGui::EndDisabled();
    }

    ImGui::SameLine();
    const char* recordButtonText = isRecording_ ? "Stop Recording" : "Record";
    if (ImGui::Button(recordButtonText)) {
        record();
    }

    ImGui::SameLine();
    bool offlineRendering = sequencer.offlineRendering();
    if (ImGui::Checkbox("Offline Rendering", &offlineRendering)) {
        sequencer.offlineRendering(offlineRendering);
    }

    // Position slider
    ImGui::Text("Position:");
    if (ImGui::SliderFloat("##Position", &playbackPosition_, 0.0f, playbackLength_, "%.1f s")) {
        std::cout << "Seeking to position: " << playbackPosition_ << std::endl;
    }

    // Time display
    int currentMin = static_cast<int>(playbackPosition_) / 60;
    int currentSec = static_cast<int>(playbackPosition_) % 60;
    int totalMin = static_cast<int>(playbackLength_) / 60;
    int totalSecTotal = static_cast<int>(playbackLength_) % 60;

    ImGui::Text("Time: %02d:%02d / %02d:%02d", currentMin, currentSec, totalMin, totalSecTotal);

    ImGui::Text("Master Volume:");
    if (ImGui::SliderFloat("##Volume", &volume_, 0.0f, 1.0f, "%.2f")) {
        std::cout << "Volume changed to: " << volume_ << std::endl;
    }

    // Mute button
    static bool isMuted = false;
    if (ImGui::Checkbox("Mute", &isMuted)) {
        std::cout << "Mute state: " << (isMuted ? "ON" : "OFF") << std::endl;
    }
}

void MainWindow::renderInstanceControl() {
    auto& sequencer = uapmd::AppModel::instance().sequencer();

    if (!pluginWindowsPendingClose_.empty()) {
        for (auto id : pluginWindowsPendingClose_) {
            sequencer.getPluginInstance(id)->destroyUI();
            pluginWindows_.erase(id);
            pluginWindowEmbedded_.erase(id);
            pluginWindowBounds_.erase(id);
            pluginWindowResizeIgnore_.erase(id);
        }
        pluginWindowsPendingClose_.clear();
    }

    if (ImGui::Button("Refresh Instances")) {
        refreshInstances();
    }

    ImGui::Text("Active Instances:");

    // Group instances by track
    std::map<int32_t, std::vector<int32_t>> instancesByTrack;
    std::vector<int32_t> unassignedInstances;

    for (int32_t instanceId : instances_) {
        int32_t trackIndex = sequencer.findTrackIndexForInstance(instanceId);
        if (trackIndex >= 0) {
            instancesByTrack[trackIndex].push_back(instanceId);
        } else {
            unassignedInstances.push_back(instanceId);
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
                int32_t instanceId = trackInstances[i];
                std::string pluginName = sequencer.getPluginName(instanceId);
                std::string pluginFormat = sequencer.getPluginFormat(instanceId);
                auto* instance = sequencer.getPluginInstance(instanceId);

                ImGui::TableNextRow();

                // Track column - only show for first plugin in track
                ImGui::TableSetColumnIndex(0);
                if (i == 0) {
                    ImGui::Text("Track %d", trackIndex + 1);
                }

                // Plugin name column
                ImGui::TableSetColumnIndex(1);
                // Indent chained effects
                if (i > 0) {
                    ImGui::Indent(20.0f);
                }
                ImGui::Text("%s", pluginName.c_str());
                if (i > 0) {
                    ImGui::Unindent(20.0f);
                }

                // Format column
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%s", pluginFormat.c_str());

                // UMP Device Name column
                ImGui::TableSetColumnIndex(3);
                {
                    // Initialize buffer if needed
                    if (umpDeviceNameBuffers_.find(instanceId) == umpDeviceNameBuffers_.end()) {
                        umpDeviceNameBuffers_[instanceId] = {};
                        // Set default name from plugin
                        std::string defaultName = std::format("{} [{}]", pluginName, pluginFormat);
                        std::strncpy(umpDeviceNameBuffers_[instanceId].data(), defaultName.c_str(),
                                    umpDeviceNameBuffers_[instanceId].size() - 1);
                    }

                    // Find if device exists and is running
                    bool deviceRunning = false;
                    auto* deviceController = uapmd::AppModel::instance().deviceController();
                    if (deviceController) {
                        std::lock_guard lock(devicesMutex_);
                        for (auto& entry : devices_) {
                            auto state = entry.state;
                            std::lock_guard guard(state->mutex);
                            if (state->pluginInstances.count(instanceId) > 0) {
                                deviceRunning = state->running;
                                break;
                            }
                        }
                    }

                    // Disable textbox if device is running
                    if (deviceRunning) {
                        ImGui::BeginDisabled();
                    }

                    std::string inputId = "##ump_name_" + std::to_string(instanceId);
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    ImGui::InputText(inputId.c_str(), umpDeviceNameBuffers_[instanceId].data(),
                                    umpDeviceNameBuffers_[instanceId].size());

                    if (deviceRunning) {
                        ImGui::EndDisabled();
                    }
                }

                // Actions context menu button
                ImGui::TableSetColumnIndex(4);
                std::string menuButtonId = "Actions##menu" + std::to_string(instanceId);
            std::string popupId = "ActionsPopup##" + std::to_string(instanceId);

            if (ImGui::Button(menuButtonId.c_str())) {
                ImGui::OpenPopup(popupId.c_str());
            }

            if (ImGui::BeginPopup(popupId.c_str())) {
                bool hasUI = instance->hasUISupport();
                bool isVisible = instance->isUIVisible();

                // Show/Hide UI menu item
                if (!hasUI) {
                    ImGui::BeginDisabled();
                }

                const char* uiMenuText = isVisible ? "Hide UI" : "Show UI";
                if (ImGui::MenuItem(uiMenuText)) {
                    if (hasUI) {
                        if (isVisible) {
                            instance->hideUI();
                            auto windowIt = pluginWindows_.find(instanceId);
                            if (windowIt != pluginWindows_.end()) windowIt->second->show(false);
                        } else {
                            // Create container window if needed
                            auto windowIt = pluginWindows_.find(instanceId);
                            remidy::gui::ContainerWindow* container = nullptr;
                            if (windowIt == pluginWindows_.end()) {
                                std::string windowTitle = pluginName + " (" + pluginFormat + ")";
                                auto w = remidy::gui::ContainerWindow::create(windowTitle.c_str(), 800, 600, [this, instanceId]() {
                                    onPluginWindowClosed(instanceId);
                                });
                                container = w.get();
                                pluginWindows_[instanceId] = std::move(w);
                                pluginWindowBounds_[instanceId] = remidy::gui::Bounds{100, 100, 800, 600};
                            } else {
                                container = windowIt->second.get();
                                if (pluginWindowBounds_.find(instanceId) == pluginWindowBounds_.end())
                                    pluginWindowBounds_[instanceId] = remidy::gui::Bounds{100, 100, 800, 600};
                            }

                            if (!container) {
                                std::cout << "Failed to create container window for instance " << instanceId << std::endl;
                            } else {
                                container->show(true);
                                void* parentHandle = container->getHandle();

                                // Check if plugin UI has been created
                                bool pluginUIExists = (pluginWindowEmbedded_.find(instanceId) != pluginWindowEmbedded_.end());

                                if (!pluginUIExists) {
                                    // First time: create plugin UI with resize handler
                                    if (!instance->createUI(false, parentHandle,
                                        [this, instanceId](uint32_t w, uint32_t h){ return handlePluginResizeRequest(instanceId, w, h); })) {
                                        container->show(false);
                                        pluginWindows_.erase(instanceId);
                                        std::cout << "Failed to create plugin UI for instance " << instanceId << std::endl;
                                    } else {
                                        pluginWindowEmbedded_[instanceId] = true;
                                    }
                                }

                                // Show the plugin UI (whether just created or already exists)
                                if (pluginUIExists || pluginWindowEmbedded_[instanceId]) {
                                    if (!instance->showUI()) {
                                        std::cout << "Failed to show plugin UI for instance " << instanceId << std::endl;
                                    }
                                }
                            }
                        }
                    }
                }

                if (!hasUI) {
                    ImGui::EndDisabled();
                }

                // Details menu item
                auto detailsIt = detailsWindows_.find(instanceId);
                bool detailsVisible = (detailsIt != detailsWindows_.end() && detailsIt->second.visible);
                const char* detailsMenuText = detailsVisible ? "Hide Details" : "Show Details";

                if (ImGui::MenuItem(detailsMenuText)) {
                    if (detailsVisible) {
                        hideDetailsWindow(instanceId);
                    } else {
                        showDetailsWindow(instanceId);
                    }
                }

                // Enable/Disable UMP device menu items
                auto* deviceController = uapmd::AppModel::instance().deviceController();
                if (deviceController) {
                    std::shared_ptr<uapmd::UapmdMidiDevice> foundDevice;
                    std::shared_ptr<DeviceState> foundDeviceState;
                    bool hasDevice = false;

                    // Find device associated with this instance
                    {
                        std::lock_guard lock(devicesMutex_);
                        for (auto& entry : devices_) {
                            auto state = entry.state;
                            std::lock_guard guard(state->mutex);
                            if (state->pluginInstances.count(instanceId) > 0) {
                                foundDevice = state->device;
                                foundDeviceState = state;
                                hasDevice = true;
                                break;
                            }
                        }
                    }

                    if (hasDevice && foundDeviceState) {
                        std::lock_guard guard(foundDeviceState->mutex);
                        const char* deviceMenuText = foundDeviceState->running ? "Disable UMP Device" : "Enable UMP Device";

                        if (foundDeviceState->instantiating) {
                            ImGui::BeginDisabled();
                        }

                        if (ImGui::MenuItem(deviceMenuText)) {
                            if (foundDeviceState->running) {
                                foundDevice->stop();
                                foundDeviceState->running = false;
                                foundDeviceState->statusMessage = "Stopped";
                                std::cout << "Disabled UMP device for instance: " << instanceId << std::endl;
                            } else {
                                // Update device name from textbox before starting
                                if (umpDeviceNameBuffers_.find(instanceId) != umpDeviceNameBuffers_.end()) {
                                    foundDeviceState->label = std::string(umpDeviceNameBuffers_[instanceId].data());
                                }

                                auto statusCode = foundDevice->start();
                                if (statusCode == 0) {
                                    foundDeviceState->running = true;
                                    foundDeviceState->statusMessage = "Running";
                                    foundDeviceState->hasError = false;
                                    std::cout << "Enabled UMP device for instance: " << instanceId << std::endl;
                                } else {
                                    foundDeviceState->statusMessage = std::format("Start failed (status {})", statusCode);
                                    foundDeviceState->hasError = true;
                                    std::cout << "Failed to enable UMP device (status " << statusCode << ")" << std::endl;
                                }
                            }
                        }

                        if (foundDeviceState->instantiating) {
                            ImGui::EndDisabled();
                        }
                    }
                    // Note: If no device exists, don't show Enable/Disable options
                    // Devices are created when plugins are instantiated
                }

                ImGui::Separator();

                // Save State menu item
                if (ImGui::MenuItem("Save State")) {
                    savePluginState(instanceId);
                }

                // Load State menu item
                if (ImGui::MenuItem("Load State")) {
                    loadPluginState(instanceId);
                }

                ImGui::Separator();

                // Remove menu item
                if (ImGui::MenuItem("Remove")) {
                    // Hide and cleanup UI if it's open
                    if (instance->hasUISupport() && instance->isUIVisible()) {
                        instance->hideUI();
                        auto windowIt = pluginWindows_.find(instanceId);
                        if (windowIt != pluginWindows_.end()) {
                            windowIt->second->show(false);
                        }
                    }

                    // Cleanup plugin UI resources
                    instance->destroyUI();
                    pluginWindows_.erase(instanceId);
                    pluginWindowEmbedded_.erase(instanceId);
                    pluginWindowBounds_.erase(instanceId);
                    pluginWindowResizeIgnore_.erase(instanceId);

                    // Cleanup details window if open
                    auto detailsIt = detailsWindows_.find(instanceId);
                    if (detailsIt != detailsWindows_.end()) {
                        detailsWindows_.erase(detailsIt);
                    }

                    // Remove the plugin instance
                    uapmd::AppModel::instance().removePluginInstance(instanceId);

                    // Refresh the instance list
                    refreshInstances();

                    std::cout << "Removed plugin instance: " << instanceId << std::endl;
                    ImGui::CloseCurrentPopup();
                }

                ImGui::EndPopup();
            }
            }
        }

        // Render unassigned instances (not grouped by track yet)
        for (int32_t instanceId : unassignedInstances) {
            std::string pluginName = sequencer.getPluginName(instanceId);
            std::string pluginFormat = sequencer.getPluginFormat(instanceId);
            auto* instance = sequencer.getPluginInstance(instanceId);

            ImGui::TableNextRow();

            // Track column
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("-");

            // Plugin name column
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%s", pluginName.c_str());

            // Format column
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%s", pluginFormat.c_str());

            // UMP Device Name column
            ImGui::TableSetColumnIndex(3);
            {
                // Initialize buffer if needed
                if (umpDeviceNameBuffers_.find(instanceId) == umpDeviceNameBuffers_.end()) {
                    umpDeviceNameBuffers_[instanceId] = {};
                    // Set default name from plugin
                    std::string defaultName = std::format("{} [{}]", pluginName, pluginFormat);
                    std::strncpy(umpDeviceNameBuffers_[instanceId].data(), defaultName.c_str(),
                                umpDeviceNameBuffers_[instanceId].size() - 1);
                }

                // Find if device exists and is running
                bool deviceRunning = false;
                auto* deviceController = uapmd::AppModel::instance().deviceController();
                if (deviceController) {
                    std::lock_guard lock(devicesMutex_);
                    for (auto& entry : devices_) {
                        auto state = entry.state;
                        std::lock_guard guard(state->mutex);
                        if (state->pluginInstances.count(instanceId) > 0) {
                            deviceRunning = state->running;
                            break;
                        }
                    }
                }

                // Disable textbox if device is running
                if (deviceRunning) {
                    ImGui::BeginDisabled();
                }

                std::string inputId = "##ump_name_" + std::to_string(instanceId);
                ImGui::SetNextItemWidth(-FLT_MIN);
                ImGui::InputText(inputId.c_str(), umpDeviceNameBuffers_[instanceId].data(),
                                umpDeviceNameBuffers_[instanceId].size());

                if (deviceRunning) {
                    ImGui::EndDisabled();
                }
            }

            // Actions context menu button (copied from above)
            ImGui::TableSetColumnIndex(4);
            std::string menuButtonId = "Actions##menu" + std::to_string(instanceId);
            std::string popupId = "ActionsPopup##" + std::to_string(instanceId);

            if (ImGui::Button(menuButtonId.c_str())) {
                ImGui::OpenPopup(popupId.c_str());
            }

            if (ImGui::BeginPopup(popupId.c_str())) {
                bool hasUI = instance->hasUISupport();
                bool isVisible = instance->isUIVisible();

                if (!hasUI) {
                    ImGui::BeginDisabled();
                }

                const char* uiMenuText = isVisible ? "Hide UI" : "Show UI";
                if (ImGui::MenuItem(uiMenuText)) {
                    if (hasUI) {
                        if (isVisible) {
                            instance->hideUI();
                            auto windowIt = pluginWindows_.find(instanceId);
                            if (windowIt != pluginWindows_.end()) windowIt->second->show(false);
                        } else {
                            auto windowIt = pluginWindows_.find(instanceId);
                            remidy::gui::ContainerWindow* container = nullptr;
                            if (windowIt == pluginWindows_.end()) {
                                std::string windowTitle = pluginName + " (" + pluginFormat + ")";
                                auto w = remidy::gui::ContainerWindow::create(windowTitle.c_str(), 800, 600, [this, instanceId]() {
                                    onPluginWindowClosed(instanceId);
                                });
                                container = w.get();
                                pluginWindows_[instanceId] = std::move(w);
                                pluginWindowBounds_[instanceId] = remidy::gui::Bounds{100, 100, 800, 600};
                            } else {
                                container = windowIt->second.get();
                                if (pluginWindowBounds_.find(instanceId) == pluginWindowBounds_.end())
                                    pluginWindowBounds_[instanceId] = remidy::gui::Bounds{100, 100, 800, 600};
                            }

                            if (!container) {
                                std::cout << "Failed to create container window for instance " << instanceId << std::endl;
                            } else {
                                container->show(true);
                                void* parentHandle = container->getHandle();
                                bool pluginUIExists = (pluginWindowEmbedded_.find(instanceId) != pluginWindowEmbedded_.end());

                                if (!pluginUIExists) {
                                    if (!instance->createUI(false, parentHandle,
                                        [this, instanceId](uint32_t w, uint32_t h){ return handlePluginResizeRequest(instanceId, w, h); })) {
                                        container->show(false);
                                        pluginWindows_.erase(instanceId);
                                        std::cout << "Failed to create plugin UI for instance " << instanceId << std::endl;
                                    } else {
                                        pluginWindowEmbedded_[instanceId] = true;
                                    }
                                }

                                if (pluginUIExists || pluginWindowEmbedded_[instanceId]) {
                                    if (!instance->showUI()) {
                                        std::cout << "Failed to show plugin UI for instance " << instanceId << std::endl;
                                    }
                                }
                            }
                        }
                    }
                }

                if (!hasUI) {
                    ImGui::EndDisabled();
                }

                auto detailsIt = detailsWindows_.find(instanceId);
                bool detailsVisible = (detailsIt != detailsWindows_.end() && detailsIt->second.visible);
                const char* detailsMenuText = detailsVisible ? "Hide Details" : "Show Details";

                if (ImGui::MenuItem(detailsMenuText)) {
                    if (detailsVisible) {
                        hideDetailsWindow(instanceId);
                    } else {
                        showDetailsWindow(instanceId);
                    }
                }

                // Enable/Disable UMP device menu items (same as track instances)
                auto* deviceController = uapmd::AppModel::instance().deviceController();
                if (deviceController) {
                    std::shared_ptr<uapmd::UapmdMidiDevice> foundDevice;
                    std::shared_ptr<DeviceState> foundDeviceState;

                    {
                        std::lock_guard lock(devicesMutex_);
                        for (auto& entry : devices_) {
                            auto state = entry.state;
                            std::lock_guard guard(state->mutex);
                            if (state->pluginInstances.count(instanceId) > 0) {
                                foundDevice = state->device;
                                foundDeviceState = state;
                                break;
                            }
                        }
                    }

                    if (foundDevice && foundDeviceState) {
                        std::lock_guard guard(foundDeviceState->mutex);
                        const char* deviceMenuText = foundDeviceState->running ? "Disable UMP Device" : "Enable UMP Device";

                        if (foundDeviceState->instantiating) {
                            ImGui::BeginDisabled();
                        }

                        if (ImGui::MenuItem(deviceMenuText)) {
                            if (foundDeviceState->running) {
                                foundDevice->stop();
                                foundDeviceState->running = false;
                                foundDeviceState->statusMessage = "Stopped";
                            } else {
                                // Update device name from textbox before starting
                                if (umpDeviceNameBuffers_.find(instanceId) != umpDeviceNameBuffers_.end()) {
                                    foundDeviceState->label = std::string(umpDeviceNameBuffers_[instanceId].data());
                                }

                                auto statusCode = foundDevice->start();
                                if (statusCode == 0) {
                                    foundDeviceState->running = true;
                                    foundDeviceState->statusMessage = "Running";
                                    foundDeviceState->hasError = false;
                                } else {
                                    foundDeviceState->statusMessage = std::format("Start failed (status {})", statusCode);
                                    foundDeviceState->hasError = true;
                                }
                            }
                        }

                        if (foundDeviceState->instantiating) {
                            ImGui::EndDisabled();
                        }
                    }
                }

                ImGui::Separator();

                if (ImGui::MenuItem("Save State")) {
                    savePluginState(instanceId);
                }

                if (ImGui::MenuItem("Load State")) {
                    loadPluginState(instanceId);
                }

                ImGui::Separator();

                if (ImGui::MenuItem("Remove")) {
                    if (instance->hasUISupport() && instance->isUIVisible()) {
                        instance->hideUI();
                        auto windowIt = pluginWindows_.find(instanceId);
                        if (windowIt != pluginWindows_.end()) {
                            windowIt->second->show(false);
                        }
                    }

                    instance->destroyUI();
                    pluginWindows_.erase(instanceId);
                    pluginWindowEmbedded_.erase(instanceId);
                    pluginWindowBounds_.erase(instanceId);
                    pluginWindowResizeIgnore_.erase(instanceId);

                    auto detailsIt = detailsWindows_.find(instanceId);
                    if (detailsIt != detailsWindows_.end()) {
                        detailsWindows_.erase(detailsIt);
                    }

                    uapmd::AppModel::instance().removePluginInstance(instanceId);
                    refreshInstances();

                    std::cout << "Removed plugin instance: " << instanceId << std::endl;
                    ImGui::CloseCurrentPopup();
                }

                ImGui::EndPopup();
            }
        }

        ImGui::EndTable();
    }
}

void MainWindow::refreshDeviceList() {
    inputDevices_.clear();
    outputDevices_.clear();

    auto manager = uapmd::AudioIODeviceManager::instance();
    auto devices = manager->devices();

    for (auto& d : devices) {
        if (d.directions & AudioIODirections::Input) {
            inputDevices_.push_back(d.name);
        }
        if (d.directions & AudioIODirections::Output) {
            outputDevices_.push_back(d.name);
        }
    }

    // Reset selection if out of bounds
    if (selectedInputDevice_ >= static_cast<int>(inputDevices_.size())) {
        selectedInputDevice_ = 0;
    }
    if (selectedOutputDevice_ >= static_cast<int>(outputDevices_.size())) {
        selectedOutputDevice_ = 0;
    }
}

void MainWindow::applyDeviceSettings() {
    // TODO: Apply settings to the actual audio system
    // This would typically involve:
    // - Stopping current audio
    // - Reconfiguring the audio system with new settings
    // - Restarting audio

    std::string inputDevice = selectedInputDevice_ < inputDevices_.size() ? inputDevices_[selectedInputDevice_] : "";
    std::string outputDevice = selectedOutputDevice_ < outputDevices_.size() ? outputDevices_[selectedOutputDevice_] : "";

    std::cout << std::format("Applied audio settings: Input={}, Output={}, SR={}, BS={}",
                            inputDevice, outputDevice, sampleRate_, bufferSize_) << std::endl;
}

void MainWindow::play() {
    auto& sequencer = uapmd::AppModel::instance().sequencer();
    sequencer.startPlayback();
    isPlaying_ = true;
    isPaused_ = false;
}

void MainWindow::stop() {
    auto& sequencer = uapmd::AppModel::instance().sequencer();
    sequencer.stopPlayback();
    isPlaying_ = false;
    isPaused_ = false;
    playbackPosition_ = 0.0f;
}

void MainWindow::pause() {
    auto& sequencer = uapmd::AppModel::instance().sequencer();
    sequencer.pausePlayback();

    isPaused_ = true;
}

void MainWindow::resume() {
    auto& sequencer = uapmd::AppModel::instance().sequencer();
    sequencer.resumePlayback();

    isPaused_ = false;
}

void MainWindow::record() {
    isRecording_ = !isRecording_;

    if (isRecording_) {
        std::cout << "Starting recording" << std::endl;
    } else {
        std::cout << "Stopping recording" << std::endl;
    }
}

void MainWindow::loadFile() {
    auto selection = pfd::open_file(
        "Select Audio File",
        ".",
        { "Audio Files", "*.wav *.flac *.ogg",
          "WAV Files", "*.wav",
          "FLAC Files", "*.flac",
          "OGG Files", "*.ogg",
          "All Files", "*" }
    );

    if (selection.result().empty()) {
        return; // User cancelled
    }

    std::string filepath = selection.result()[0];

    // Determine file type by extension and create appropriate reader
    std::unique_ptr<choc::audio::AudioFileReader> reader;

    auto extension = filepath.substr(filepath.find_last_of('.') + 1);
    std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);

    if (extension == "wav") {
        reader = choc::audio::WAVAudioFileFormat<true>().createReader(filepath);
    } else if (extension == "flac") {
        reader = choc::audio::FLACAudioFileFormat<true>().createReader(filepath);
    } else if (extension == "ogg") {
        reader = choc::audio::OggAudioFileFormat<true>().createReader(filepath);
    } else {
        // Try all formats as fallback
        reader = choc::audio::WAVAudioFileFormat<true>().createReader(filepath);
        if (!reader)
            reader = choc::audio::FLACAudioFileFormat<true>().createReader(filepath);
        if (!reader)
            reader = choc::audio::OggAudioFileFormat<true>().createReader(filepath);
    }

    if (!reader) {
        pfd::message("Load Failed",
                    "Could not load audio file: " + filepath + "\nSupported formats: WAV, FLAC, OGG",
                    pfd::choice::ok,
                    pfd::icon::error);
        return;
    }

    auto& sequencer = uapmd::AppModel::instance().sequencer();
    sequencer.loadAudioFile(std::move(reader));

    currentFile_ = filepath;
    playbackLength_ = static_cast<float>(sequencer.audioFileDurationSeconds());
    playbackPosition_ = 0.0f;

    // Add to recent files if not already there
    auto it = std::find(recentFiles_.begin(), recentFiles_.end(), currentFile_);
    if (it == recentFiles_.end()) {
        recentFiles_.insert(recentFiles_.begin(), currentFile_);
        if (recentFiles_.size() > 10) { // Keep only 10 recent files
            recentFiles_.pop_back();
        }
    }

    std::cout << "File loaded: " << currentFile_ << std::endl;
}

void MainWindow::refreshInstances() {
    instances_.clear();

    // Get actual instance list from sequencer
    auto& sequencer = uapmd::AppModel::instance().sequencer();
    instances_ = sequencer.getInstanceIds();

    for (auto it = pluginWindows_.begin(); it != pluginWindows_.end();) {
        if (std::find(instances_.begin(), instances_.end(), it->first) == instances_.end()) {
            it = pluginWindows_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = pluginWindowEmbedded_.begin(); it != pluginWindowEmbedded_.end();) {
        if (std::find(instances_.begin(), instances_.end(), it->first) == instances_.end()) {
            it = pluginWindowEmbedded_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = pluginWindowBounds_.begin(); it != pluginWindowBounds_.end();) {
        if (std::find(instances_.begin(), instances_.end(), it->first) == instances_.end()) {
            it = pluginWindowBounds_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = detailsWindows_.begin(); it != detailsWindows_.end();) {
        if (std::find(instances_.begin(), instances_.end(), it->first) == instances_.end()) {
            it = detailsWindows_.erase(it);
        } else {
            ++it;
        }
    }
    std::vector<int32_t> resizeIgnoreRemove{};
    resizeIgnoreRemove.reserve(pluginWindowResizeIgnore_.size());
    for (auto id : pluginWindowResizeIgnore_) {
        if (std::find(instances_.begin(), instances_.end(), id) == instances_.end())
            resizeIgnoreRemove.push_back(id);
    }
    for (auto id : resizeIgnoreRemove)
        pluginWindowResizeIgnore_.erase(id);

}

void MainWindow::refreshParameters(int32_t instanceId, DetailsWindowState& state) {
    state.parameters.clear();
    state.parameterValues.clear();
    state.parameterValueStrings.clear();

    auto* pal = uapmd::AppModel::instance().sequencer().getPluginInstance(instanceId);
    if (!pal) {
        return;
    }

    state.parameters = pal->parameterMetadataList();
    state.parameterValues.resize(state.parameters.size());
    state.parameterValueStrings.resize(state.parameters.size());
    auto* parameterSupport = pal->parameterSupport();
    for (size_t i = 0; i < state.parameters.size(); ++i) {
        double initialValue = state.parameters[i].defaultPlainValue;
        if (parameterSupport) {
            double queriedValue = initialValue;
            auto status = parameterSupport->getParameter(state.parameters[i].index, &queriedValue);
            if (status == remidy::StatusCode::OK) {
                initialValue = queriedValue;
            }
        }
        state.parameterValues[i] = static_cast<float>(initialValue);
        updateParameterValueString(i, instanceId, state);
    }

    applyParameterUpdates(instanceId, state);
}

void MainWindow::refreshPresets(int32_t instanceId, DetailsWindowState& state) {
    state.presets.clear();
    state.selectedPreset = -1;

    auto* pal = uapmd::AppModel::instance().sequencer().getPluginInstance(instanceId);
    if (!pal) {
        return;
    }

    state.presets = pal->presetMetadataList();
}

void MainWindow::loadSelectedPreset(int32_t instanceId, DetailsWindowState& state) {
    if (state.selectedPreset < 0 || state.selectedPreset >= static_cast<int>(state.presets.size())) {
        return;
    }

    auto* pal = uapmd::AppModel::instance().sequencer().getPluginInstance(instanceId);
    if (!pal) {
        return;
    }

    const auto& preset = state.presets[state.selectedPreset];
    pal->loadPreset(preset.index);
    std::cout << "Loading preset " << preset.name << " for instance " << instanceId << std::endl;

    refreshParameters(instanceId, state);
}

void MainWindow::updateParameterValueString(size_t parameterIndex, int32_t instanceId, DetailsWindowState& state) {
    if (parameterIndex >= state.parameters.size()) {
        return;
    }

    auto* pal = uapmd::AppModel::instance().sequencer().getPluginInstance(instanceId);
    if (!pal) {
        return;
    }

    auto& param = state.parameters[parameterIndex];
    if (state.parameterValueStrings.size() <= parameterIndex) {
        state.parameterValueStrings.resize(parameterIndex + 1);
    }

    state.parameterValueStrings[parameterIndex] = pal->getParameterValueString(
        param.index, state.parameterValues[parameterIndex]);
}

void MainWindow::applyParameterUpdates(int32_t instanceId, DetailsWindowState& state) {
    auto& sequencer = uapmd::AppModel::instance().sequencer();
    auto updates = sequencer.getParameterUpdates(instanceId);
    for (const auto& update : updates) {
        for (size_t i = 0; i < state.parameters.size(); ++i) {
            if (state.parameters[i].index == update.parameterIndex) {
                state.parameterValues[i] = static_cast<float>(update.value);
                updateParameterValueString(i, instanceId, state);
                break;
            }
        }
    }
}

void MainWindow::renderParameterControls(int32_t instanceId, DetailsWindowState& state) {
    auto& sequencer = uapmd::AppModel::instance().sequencer();
    auto* pal = sequencer.getPluginInstance(instanceId);
    if (!pal) {
        return;
    }

    ImGui::Checkbox("Reflect Event Out", &state.reflectEventOut);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("When enabled, parameter changes from plugin output events\nwill be reflected in the UI controls");
    }

    if (state.reflectEventOut) {
        applyParameterUpdates(instanceId, state);
    }

    ImGui::InputText("Filter Parameters", state.parameterFilter, sizeof(state.parameterFilter));

    const ImGuiTableFlags parameterTableFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable |
                                                ImGuiTableFlags_Sortable | ImGuiTableFlags_SortTristate;
    if (ImGui::BeginTable("ParameterTable", 5, parameterTableFlags)) {
        ImGui::TableSetupColumn("Index", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort |
                                               ImGuiTableColumnFlags_PreferSortAscending,
                                30.0f);
        ImGui::TableSetupColumn("Stable ID", ImGuiTableColumnFlags_WidthFixed, 50.0f);
        ImGui::TableSetupColumn("Parameter", ImGuiTableColumnFlags_WidthStretch, 0.0f, 0);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 180.0f);
        ImGui::TableSetupColumn("Default", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableHeadersRow();

        std::string filter = state.parameterFilter;
        std::transform(filter.begin(), filter.end(), filter.begin(), ::tolower);

        std::vector<size_t> visibleParameterIndices;
        visibleParameterIndices.reserve(state.parameters.size());

        for (size_t i = 0; i < state.parameters.size(); ++i) {
            auto& param = state.parameters[i];

            if (param.hidden || (!param.automatable && !param.discrete))
                continue;

            if (!filter.empty()) {
                std::string name = param.name;
                std::string stableId = param.stableId;
                std::transform(name.begin(), name.end(), name.begin(), ::tolower);
                std::transform(stableId.begin(), stableId.end(), stableId.begin(), ::tolower);
                if (name.find(filter) == std::string::npos && stableId.find(filter) == std::string::npos) {
                    continue;
                }
            }

            visibleParameterIndices.push_back(i);
        }

        auto compareByColumn = [&](size_t lhs, size_t rhs, int columnIndex) {
            const auto& leftParam = state.parameters[lhs];
            const auto& rightParam = state.parameters[rhs];
            switch (columnIndex) {
            case 0:
                if (leftParam.index < rightParam.index)
                    return -1;
                if (leftParam.index > rightParam.index)
                    return 1;
                return 0;
            case 1:
                if (leftParam.stableId < rightParam.stableId)
                    return -1;
                if (leftParam.stableId > rightParam.stableId)
                    return 1;
                return 0;
            case 2:
                if (leftParam.name < rightParam.name)
                    return -1;
                if (leftParam.name > rightParam.name)
                    return 1;
                return 0;
            case 3:
                if (state.parameterValues[lhs] < state.parameterValues[rhs])
                    return -1;
                if (state.parameterValues[lhs] > state.parameterValues[rhs])
                    return 1;
                return 0;
            case 4:
                if (leftParam.defaultPlainValue < rightParam.defaultPlainValue)
                    return -1;
                if (leftParam.defaultPlainValue > rightParam.defaultPlainValue)
                    return 1;
                return 0;
            default:
                return 0;
            }
        };

        if (ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs()) {
            if (!visibleParameterIndices.empty() && sortSpecs->SpecsCount > 0) {
                std::stable_sort(visibleParameterIndices.begin(), visibleParameterIndices.end(),
                                 [&](size_t lhs, size_t rhs) {
                                     for (int n = 0; n < sortSpecs->SpecsCount; ++n) {
                                         const ImGuiTableColumnSortSpecs& spec = sortSpecs->Specs[n];
                                         int delta = compareByColumn(lhs, rhs, spec.ColumnIndex);
                                         if (delta != 0) {
                                             return spec.SortDirection == ImGuiSortDirection_Ascending ? (delta < 0)
                                                                                                       : (delta > 0);
                                         }
                                     }
                                     return lhs < rhs;
                                 });
            }
            sortSpecs->SpecsDirty = false;
        }

        for (size_t paramIndex : visibleParameterIndices) {
            auto& param = state.parameters[paramIndex];

            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            ImGui::Text("%u", param.index);

            ImGui::TableNextColumn();
            if (param.stableId.empty()) {
                ImGui::Text("(empty)");
            } else {
                ImGui::Text("%s", param.stableId.c_str());
            }

            ImGui::TableNextColumn();
            ImGui::Text("%s", param.name.c_str());

            ImGui::TableNextColumn();
            std::string controlId = "##" + std::to_string(param.index);

            bool parameterChanged = false;
            const bool hasDiscreteCombo = !param.namedValues.empty();
            const char* format = state.parameterValueStrings[paramIndex].empty()
                                     ? "%.3f"
                                     : state.parameterValueStrings[paramIndex].c_str();

            float sliderWidth = ImGui::GetContentRegionAvail().x;
            float comboButtonWidth = 0.0f;
            float comboSpacing = 0.0f;
            if (hasDiscreteCombo) {
                comboButtonWidth = ImGui::GetFrameHeight();
                comboSpacing = ImGui::GetStyle().ItemInnerSpacing.x;
                sliderWidth = std::max(20.0f, sliderWidth - (comboButtonWidth + comboSpacing));
            }

            ImGui::SetNextItemWidth(sliderWidth);
            if (ImGui::SliderFloat(controlId.c_str(), &state.parameterValues[paramIndex],
                                   static_cast<float>(param.minPlainValue),
                                   static_cast<float>(param.maxPlainValue), format)) {
                parameterChanged = true;
            }

            ImVec2 sliderMin = ImGui::GetItemRectMin();
            ImVec2 sliderMax = ImGui::GetItemRectMax();

            if (hasDiscreteCombo) {
                ImGui::SameLine(0.0f, comboSpacing);
                std::string comboButtonId = controlId + "_combo";
                std::string comboPopupId = controlId + "_popup";
                const bool popupOpen = ImGui::IsPopupOpen(comboPopupId.c_str(), ImGuiPopupFlags_None);
                bool requestPopupClose = false;

                if (popupOpen) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
                }

                if (ImGui::ArrowButton(comboButtonId.c_str(), ImGuiDir_Down)) {
                    if (popupOpen) {
                        requestPopupClose = true;
                    } else {
                        ImGui::OpenPopup(comboPopupId.c_str());
                    }
                }

                if (popupOpen) {
                    ImGui::PopStyleColor(2);
                }

                ImGui::SetNextWindowPos(sliderMin);
                ImGui::SetNextWindowSize(ImVec2(sliderMax.x - sliderMin.x, 0.0f));
                if (ImGui::BeginPopup(comboPopupId.c_str())) {
                    if (requestPopupClose) {
                        ImGui::CloseCurrentPopup();
                    } else {
                        const std::string& currentLabel = state.parameterValueStrings[paramIndex].empty()
                                                               ? std::to_string(state.parameterValues[paramIndex])
                                                               : state.parameterValueStrings[paramIndex];

                        if (!currentLabel.empty()) {
                            ImGui::TextUnformatted(currentLabel.c_str());
                            ImGui::Separator();
                        }

                        for (const auto& namedValue : param.namedValues) {
                            bool isSelected = (std::abs(namedValue.value - state.parameterValues[paramIndex]) < 0.0001);
                            if (ImGui::Selectable(namedValue.name.c_str(), isSelected)) {
                                state.parameterValues[paramIndex] = static_cast<float>(namedValue.value);
                                parameterChanged = true;
                                ImGui::CloseCurrentPopup();
                            }
                            if (isSelected) {
                                ImGui::SetItemDefaultFocus();
                            }
                        }
                    }
                    ImGui::EndPopup();
                }
            }

            if (parameterChanged) {
                sequencer.setParameterValue(instanceId, param.index, state.parameterValues[paramIndex]);
                updateParameterValueString(paramIndex, instanceId, state);
                std::cout << "Parameter " << param.name << " changed to " << state.parameterValues[paramIndex] << std::endl;
            }

            ImGui::TableNextColumn();
            std::string resetId = "Reset##" + std::to_string(param.index);
            if (ImGui::Button(resetId.c_str())) {
                state.parameterValues[paramIndex] = static_cast<float>(param.defaultPlainValue);
                sequencer.setParameterValue(instanceId, param.index, state.parameterValues[paramIndex]);
                updateParameterValueString(paramIndex, instanceId, state);
            }
        }

        ImGui::EndTable();
    }
}

void MainWindow::refreshPluginList() {
    availablePlugins_.clear();

    auto& catalog = uapmd::AppModel::instance().sequencer().catalog();
    auto plugins = catalog.getPlugins();

    for (auto* plugin : plugins) {
        availablePlugins_.push_back({
            .format = plugin->format(),
            .id = plugin->pluginId(),
            .name = plugin->displayName(),
            .vendor = plugin->vendorName()
        });
    }
}

void MainWindow::renderPluginSelector() {
    ImGui::InputText("Search", searchFilter_, sizeof(searchFilter_));

    if (ImGui::BeginTable("PluginTable", 4,
                          ImGuiTableFlags_Resizable | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_Sortable,
                          ImVec2(0, 300))) {
        ImGui::TableSetupColumn("Format", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Vendor", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        std::string filter = searchFilter_;
        std::transform(filter.begin(), filter.end(), filter.begin(), ::tolower);

        // Build list of visible indices after filtering
        std::vector<int> indices;
        indices.reserve(availablePlugins_.size());
        for (size_t i = 0; i < availablePlugins_.size(); ++i) {
            const auto& p = availablePlugins_[i];
            if (!filter.empty()) {
                std::string name = p.name;
                std::string vendor = p.vendor;
                std::transform(name.begin(), name.end(), name.begin(), ::tolower);
                std::transform(vendor.begin(), vendor.end(), vendor.begin(), ::tolower);
                if (name.find(filter) == std::string::npos && vendor.find(filter) == std::string::npos) {
                    continue;
                }
            }
            indices.push_back(static_cast<int>(i));
        }

        // Sort visible indices according to table sort specs
        if (!indices.empty()) {
            if (ImGuiTableSortSpecs* sort_specs = ImGui::TableGetSortSpecs()) {
                if (sort_specs->SpecsCount > 0) {
                    auto cmp = [&](int lhsIdx, int rhsIdx) {
                        const auto& a = availablePlugins_[static_cast<size_t>(lhsIdx)];
                        const auto& b = availablePlugins_[static_cast<size_t>(rhsIdx)];
                        for (int n = 0; n < sort_specs->SpecsCount; n++) {
                            const ImGuiTableColumnSortSpecs* s = &sort_specs->Specs[n];
                            int delta = 0;
                            switch (s->ColumnIndex) {
                                case 0: delta = a.format.compare(b.format); break;
                                case 1: delta = a.name.compare(b.name); break;
                                case 2: delta = a.vendor.compare(b.vendor); break;
                                case 3: delta = a.id.compare(b.id); break;
                                default: break;
                            }
                            if (delta != 0) {
                                return (s->SortDirection == ImGuiSortDirection_Ascending) ? (delta < 0) : (delta > 0);
                            }
                        }
                        // Tiebreaker to get deterministic order
                        if (int t = a.name.compare(b.name); t != 0) return t < 0;
                        if (int t = a.vendor.compare(b.vendor); t != 0) return t < 0;
                        if (int t = a.id.compare(b.id); t != 0) return t < 0;
                        return a.format < b.format;
                    };
                    std::sort(indices.begin(), indices.end(), cmp);
                }
            }
        }

        for (int sortedIndex : indices) {
            const auto& plugin = availablePlugins_[static_cast<size_t>(sortedIndex)];

            bool isSelected = (selectedPluginFormat_ == plugin.format && selectedPluginId_ == plugin.id);

            ImGui::TableNextRow();
            ImGui::TableNextColumn();

            std::string selectableId = "##" + plugin.id + "##" + std::to_string(sortedIndex);
            if (ImGui::Selectable(selectableId.c_str(), isSelected, ImGuiSelectableFlags_SpanAllColumns)) {
                selectedPluginFormat_ = plugin.format;
                selectedPluginId_ = plugin.id;
                std::cout << "[GUI] Selected plugin: format='" << plugin.format << "', id='" << plugin.id
                          << "', name='" << plugin.name << "'" << std::endl;
            }

            ImGui::SameLine();
            ImGui::Text("%s", plugin.format.c_str());

            ImGui::TableNextColumn();
            ImGui::Text("%s", plugin.name.c_str());

            ImGui::TableNextColumn();
            ImGui::Text("%s", plugin.vendor.c_str());

            ImGui::TableNextColumn();
            ImGui::Text("%s", plugin.id.c_str());
        }
        ImGui::EndTable();
                          }

    // Plugin scanning controls
    ImGui::Separator();

    bool isScanning = uapmd::AppModel::instance().isScanning();
    if (isScanning) {
        ImGui::BeginDisabled();
    }

    if (ImGui::Button("Scan Plugins")) {
        uapmd::AppModel::instance().performPluginScanning(forceRescan_);
        std::cout << "Starting plugin scanning" << std::endl;
    }

    if (isScanning) {
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::Text("Scanning...");
    } else {
        ImGui::SameLine();
        ImGui::Checkbox("Force Rescan", &forceRescan_);
    }

    ImGui::Separator();

    // Build track destination options
    std::vector<std::string> labels;
    {
        trackOptions_.clear();
        auto* deviceController = uapmd::AppModel::instance().deviceController();
        if (deviceController && deviceController->sequencer()) {
            auto tracks = deviceController->sequencer()->getTrackInfos();
            for (const auto& track : tracks) {
                TrackDestinationOption option{
                    .trackIndex = track.trackIndex,
                    .label = std::format("Track {}", track.trackIndex + 1)
                };
                trackOptions_.push_back(std::move(option));
            }
        }
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
    bool canInstantiate = !selectedPluginFormat_.empty() && !selectedPluginId_.empty();
    if (!canInstantiate) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Instantiate Plugin")) {
        // Determine track index (matching uapmd-service logic)
        int32_t trackIndex = -1;
        if (selectedTrackOption_ > 0 && static_cast<size_t>(selectedTrackOption_ - 1) < trackOptions_.size()) {
            // Use existing track
            trackIndex = trackOptions_[static_cast<size_t>(selectedTrackOption_ - 1)].trackIndex;
        }

        // Always create device (matching uapmd-service - it creates UMP device even for existing tracks)
        createDeviceForPlugin(selectedPluginFormat_, selectedPluginId_, trackIndex);
        showPluginSelector_ = false;
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

void MainWindow::showDetailsWindow(int32_t instanceId) {
    auto& sequencer = uapmd::AppModel::instance().sequencer();

    auto it = detailsWindows_.find(instanceId);
    if (it == detailsWindows_.end()) {
        // Create new details window state
        DetailsWindowState state;

        // Initialize MIDI keyboard for this instance
        state.midiKeyboard.setOctaveRange(3, 4);
        state.midiKeyboard.setKeyEventCallback([this, instanceId](int note, int velocity, bool isPressed) {
            auto& seq = uapmd::AppModel::instance().sequencer();
            // Find the track index for this instance
            auto trackIdx = seq.findTrackIndexForInstance(instanceId);
            if (trackIdx >= 0) {
                if (isPressed) {
                    seq.sendNoteOn(trackIdx, note);
                } else {
                    seq.sendNoteOff(trackIdx, note);
                }
            }
        });

        state.visible = true;
        refreshParameters(instanceId, state);
        refreshPresets(instanceId, state);
        detailsWindows_[instanceId] = std::move(state);
    } else {
        // Window already exists, just show it
        it->second.visible = true;
    }
}

void MainWindow::hideDetailsWindow(int32_t instanceId) {
    auto it = detailsWindows_.find(instanceId);
    if (it != detailsWindows_.end()) {
        it->second.visible = false;
    }
}

void MainWindow::onDetailsWindowClosed(int32_t instanceId) {
    auto it = detailsWindows_.find(instanceId);
    if (it != detailsWindows_.end()) {
        it->second.visible = false;
    }
}

void MainWindow::renderDetailsWindows() {
    auto& sequencer = uapmd::AppModel::instance().sequencer();

    for (auto& [instanceId, detailsState] : detailsWindows_) {
        if (!detailsState.visible) {
            continue;
        }

        // Create ImGui window for this instance's details
        std::string windowTitle = sequencer.getPluginName(instanceId) + " (" +
                                 sequencer.getPluginFormat(instanceId) + ") - Details###Details" +
                                 std::to_string(instanceId);

        bool windowOpen = detailsState.visible;
        ImGui::SetNextWindowSize(ImVec2(600, 500), ImGuiCond_FirstUseEver);
        if (ImGui::Begin(windowTitle.c_str(), &windowOpen)) {
            auto* instance = sequencer.getPluginInstance(instanceId);
            if (!instance) {
                ImGui::TextUnformatted("Instance is no longer available.");
            } else {
                if (detailsState.parameters.empty() && detailsState.parameterValues.empty()) {
                    refreshParameters(instanceId, detailsState);
                }
                if (detailsState.presets.empty()) {
                    refreshPresets(instanceId, detailsState);
                }

                ImGui::Text("MIDI Keyboard:");
                detailsState.midiKeyboard.render();
                ImGui::Separator();

                ImGui::Text("Presets:");
                if (detailsState.presets.empty()) {
                    ImGui::TextDisabled("No presets available for this plugin.");
                    ImGui::BeginDisabled();
                    ImGui::Button("Load Preset");
                    ImGui::EndDisabled();
                } else {
                    std::string presetPreviewLabel = "Select preset...";
                    if (detailsState.selectedPreset >= 0 &&
                        detailsState.selectedPreset < static_cast<int>(detailsState.presets.size())) {
                        presetPreviewLabel = detailsState.presets[detailsState.selectedPreset].name;
                        if (presetPreviewLabel.empty()) {
                            presetPreviewLabel = "(Unnamed preset)";
                        }
                    }

                    if (ImGui::BeginCombo("##PresetCombo", presetPreviewLabel.c_str())) {
                        for (size_t i = 0; i < detailsState.presets.size(); i++) {
                            const bool isSelected = (detailsState.selectedPreset == static_cast<int>(i));
                            std::string displayName = detailsState.presets[i].name;
                            if (displayName.empty()) {
                                displayName = "(Unnamed preset)";
                            }
                            // Ensure ImGui receives a stable ID even if the preset has no label.
                            std::string selectableLabel =
                                displayName + "##Preset" + std::to_string(i);
                            if (ImGui::Selectable(selectableLabel.c_str(), isSelected)) {
                                detailsState.selectedPreset = static_cast<int>(i);
                            }
                            if (isSelected) {
                                ImGui::SetItemDefaultFocus();
                            }
                        }
                        ImGui::EndCombo();
                    }

                    ImGui::SameLine();
                    bool canLoadPreset = detailsState.selectedPreset >= 0 &&
                                         detailsState.selectedPreset < static_cast<int>(detailsState.presets.size());
                    if (!canLoadPreset) {
                        ImGui::BeginDisabled();
                    }
                    if (ImGui::Button("Load Preset")) {
                        loadSelectedPreset(instanceId, detailsState);
                    }
                    if (!canLoadPreset) {
                        ImGui::EndDisabled();
                    }
                }

                ImGui::Separator();

                ImGui::Text("Parameters:");
                if (ImGui::BeginChild("ParametersChild", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar)) {
                    renderParameterControls(instanceId, detailsState);
                }
                ImGui::EndChild();
            }
        }
        ImGui::End();

        // Update visibility if user closed the window
        if (!windowOpen) {
            hideDetailsWindow(instanceId);
        }
    }
}

void MainWindow::savePluginState(int32_t instanceId) {
    auto& sequencer = uapmd::AppModel::instance().sequencer();

    std::string defaultFilename = std::format("{}.{}.state",
                                              sequencer.getPluginName(instanceId),
                                              sequencer.getPluginFormat(instanceId));
    std::ranges::replace(defaultFilename, ' ', '_');

    auto save = pfd::save_file(
        "Save Plugin State",
        defaultFilename,
        {"Plugin State Files", "*.state", "All Files", "*"}
    );

    std::string filepath = save.result();
    if (filepath.empty()) {
        return; // User cancelled
    }

    // Get plugin state directly from plugin instance
    auto* instance = sequencer.getPluginInstance(instanceId);
    if (!instance) {
        std::cerr << "Failed to get plugin instance" << std::endl;
        pfd::message("Save Failed",
            "Failed to get plugin instance",
            pfd::choice::ok,
            pfd::icon::error);
        return;
    }

    auto stateData = instance->saveState();
    if (stateData.empty()) {
        std::cerr << "Failed to retrieve plugin state" << std::endl;
        pfd::message("Save Failed",
            "Failed to retrieve plugin state",
            pfd::choice::ok,
            pfd::icon::error);
        return;
    }

    // Save to file as binary blob
    try {
        std::ofstream file(filepath, std::ios::binary);
        if (!file.is_open()) {
            throw std::runtime_error("Failed to open file for writing");
        }
        file.write(reinterpret_cast<const char*>(stateData.data()), stateData.size());
        file.close();

        std::cout << "Plugin state saved to: " << filepath << std::endl;
    } catch (const std::exception& ex) {
        std::cerr << "Failed to save plugin state: " << ex.what() << std::endl;
        pfd::message("Save Failed",
            std::format("Failed to save plugin state:\n{}", ex.what()),
            pfd::choice::ok,
            pfd::icon::error);
    }
}

void MainWindow::loadPluginState(int32_t instanceId) {
    // Show open file dialog
    auto open = pfd::open_file(
        "Load Plugin State",
        "",
        {"Plugin State Files", "*.state", "All Files", "*"}
    );

    auto filepaths = open.result();
    if (filepaths.empty()) {
        return; // User cancelled
    }

    std::string filepath = filepaths[0];

    // Load from file
    std::vector<uint8_t> stateData;
    try {
        std::ifstream file(filepath, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            throw std::runtime_error("Failed to open file for reading");
        }

        auto fileSize = file.tellg();
        file.seekg(0, std::ios::beg);

        stateData.resize(static_cast<size_t>(fileSize));
        file.read(reinterpret_cast<char*>(stateData.data()), fileSize);
        file.close();
    } catch (const std::exception& ex) {
        std::cerr << "Failed to load plugin state: " << ex.what() << std::endl;
        pfd::message("Load Failed",
            std::format("Failed to load plugin state:\n{}", ex.what()),
            pfd::choice::ok,
            pfd::icon::error);
        return;
    }

    // Set plugin state directly on plugin instance
    auto& sequencer = uapmd::AppModel::instance().sequencer();
    auto* instance = sequencer.getPluginInstance(instanceId);
    if (!instance) {
        std::cerr << "Failed to get plugin instance" << std::endl;
        pfd::message("Load Failed",
            "Failed to get plugin instance",
            pfd::choice::ok,
            pfd::icon::error);
        return;
    }

    instance->loadState(stateData);
    // Note: loadState doesn't return a status, so we assume success

    std::cout << "Plugin state loaded from: " << filepath << std::endl;

    // Refresh parameters to reflect the loaded state if details window is open
    auto detailsIt = detailsWindows_.find(instanceId);
    if (detailsIt != detailsWindows_.end()) {
        refreshParameters(instanceId, detailsIt->second);
    }
}

void MainWindow::createDeviceForPlugin(const std::string& format, const std::string& pluginId, int32_t trackIndex) {
    auto* deviceController = uapmd::AppModel::instance().deviceController();
    if (!deviceController) {
        std::cout << "Device controller not available" << std::endl;
        return;
    }

    // Get plugin name from catalog (matching uapmd-service logic)
    std::string pluginName;
    auto& catalog = uapmd::AppModel::instance().sequencer().catalog();
    auto plugins = catalog.getPlugins();
    for (const auto& plugin : plugins) {
        if (plugin->format() == format && plugin->pluginId() == pluginId) {
            pluginName = plugin->displayName();
            break;
        }
    }

    // Create device state (matching uapmd-service MainWindow.cpp:719-730)
    auto state = std::make_shared<DeviceState>();
    state->apiName = std::string(apiInput_);
    if (state->apiName.empty()) {
        state->apiName = "default";
    }
    state->label = std::string(deviceNameInput_);
    if (state->label.empty()) {
        state->label = std::format("{} [{}]", pluginName, format);
    }
    state->statusMessage = trackIndex >= 0 ? std::format("Adding to track {}...", trackIndex + 1)
                                           : "Instantiating plugin...";
    state->instantiating = true;

    std::string errorMessage;
    auto device = deviceController->createDevice(
        state->apiName,
        state->label,      // Use the label (device name) here!
        "UAPMD Project",  // manufacturer (matching uapmd-service)
        "0.1",            // version (matching uapmd-service)
        trackIndex,
        format,
        pluginId,
        errorMessage
    );

    if (!device) {
        std::lock_guard guard(state->mutex);
        state->statusMessage = "Plugin instantiation failed: " + errorMessage;
        state->hasError = true;
        state->instantiating = false;
        std::cout << "Failed to create virtual MIDI device: " << errorMessage << std::endl;
        return;
    }

    state->device = device;

    // Start the device (matching uapmd-service MainWindow.cpp:747-774)
    int startStatus = device->start();
    {
        std::lock_guard guard(state->mutex);
        if (startStatus == 0) {
            state->running = true;
            state->statusMessage = "Running";
            std::cout << "Virtual MIDI device started successfully: " << state->label << std::endl;
        } else {
            state->running = false;
            state->hasError = true;
            state->statusMessage = std::format("Failed to start device (status {})", startStatus);
            deviceController->removeDevice(device->instanceId());
            state->device.reset();
            state->pluginInstances.erase(device->instanceId());
            std::cout << "Failed to start virtual MIDI device (status " << startStatus << ")" << std::endl;
        }
        state->instantiating = false;

        if (startStatus == 0) {
            auto& node = state->pluginInstances[device->instanceId()];
            node.instanceId = device->instanceId();
            node.pluginName = pluginName;
            node.pluginFormat = format;
            node.pluginId = pluginId;
            node.statusMessage = std::format("Plugin ready (instance {})", node.instanceId);
            node.instantiating = false;
            node.hasError = false;
            node.trackIndex = device->trackIndex();
        }
    }

    {
        std::lock_guard lock(devicesMutex_);
        devices_.push_back(DeviceEntry{nextDeviceId_++, state});
    }

    refreshInstances();
}

void MainWindow::renderVirtualMidiDeviceManager() {
    ImGui::TextUnformatted("Audio tracks");
    std::vector<DeviceEntry> devicesCopy;
    {
        std::lock_guard lock(devicesMutex_);
        devicesCopy = devices_;
    }

    if (devicesCopy.empty()) {
        ImGui::TextDisabled("No virtual MIDI devices created yet.");
        return;
    }

    size_t removeIndex = static_cast<size_t>(-1);

    // Build rows structure (copied from uapmd-service MainWindow.cpp:384-463)
    struct PluginRow {
        int32_t instanceId{-1};
        std::string pluginName{};
        std::string pluginFormat{};
        std::string pluginId{};
        std::string status{};
        bool pluginInstantiating{false};
        bool pluginHasError{false};
        int32_t trackIndex{-1};
        std::shared_ptr<DeviceState> deviceState{};
        size_t deviceIndex{static_cast<size_t>(-1)};
        std::string deviceLabel{};
        bool deviceRunning{false};
        bool deviceInstantiating{false};
        bool deviceHasError{false};
    };

    std::map<int32_t, std::vector<PluginRow>> rowsByTrack;
    std::vector<PluginRow> pendingRows;

    auto* deviceController = uapmd::AppModel::instance().deviceController();
    auto* sequencer = deviceController ? deviceController->sequencer() : nullptr;

    for (size_t deviceIdx = 0; deviceIdx < devicesCopy.size(); ++deviceIdx) {
        auto entry = devicesCopy[deviceIdx];
        auto state = entry.state;

        std::lock_guard guard(state->mutex);
        const std::string deviceLabel = state->label.empty() ? std::format("Device {}", deviceIdx + 1) : state->label;

        if (state->pluginInstances.empty()) {
            pendingRows.push_back(PluginRow{
                -1,
                deviceLabel,
                "",
                "",
                state->statusMessage,
                state->instantiating,
                state->hasError,
                -1,
                state,
                deviceIdx,
                deviceLabel,
                state->running,
                state->instantiating,
                state->hasError
            });
            continue;
        }

        for (auto& [instanceId, pluginState] : state->pluginInstances) {
            PluginRow row{};
            row.instanceId = instanceId;
            row.pluginName = pluginState.pluginName;
            row.pluginFormat = pluginState.pluginFormat;
            row.pluginId = pluginState.pluginId;
            row.status = pluginState.statusMessage;
            row.pluginInstantiating = pluginState.instantiating;
            row.pluginHasError = pluginState.hasError;
            row.trackIndex = pluginState.trackIndex;
            row.deviceState = state;
            row.deviceIndex = deviceIdx;
            row.deviceLabel = deviceLabel;
            row.deviceRunning = state->running;
            row.deviceInstantiating = state->instantiating;
            row.deviceHasError = state->hasError;

            if (sequencer) {
                const auto trackIndex = sequencer->findTrackIndexForInstance(instanceId);
                if (trackIndex >= 0) {
                    pluginState.trackIndex = trackIndex;
                }
                row.trackIndex = pluginState.trackIndex;
            }

            if (row.trackIndex >= 0) {
                rowsByTrack[row.trackIndex].push_back(std::move(row));
            } else {
                pendingRows.push_back(std::move(row));
            }
        }
    }

    // Render row lambda (copied from uapmd-service MainWindow.cpp:465-594)
    auto renderRow = [&](PluginRow& row) -> bool {
        ImGui::TableNextRow();

        auto deviceState = row.deviceState;
        bool deviceRunning = row.deviceRunning;
        bool deviceInstantiating = row.deviceInstantiating;
        bool deviceHasError = row.deviceHasError;
        std::string pluginStatus = row.status;
        bool pluginInstantiating = row.pluginInstantiating;
        bool pluginHasError = row.pluginHasError;

        if (deviceState) {
            std::lock_guard guard(deviceState->mutex);
            deviceRunning = deviceState->running;
            deviceInstantiating = deviceState->instantiating;
            deviceHasError = deviceState->hasError;
            if (row.instanceId >= 0 && deviceState->pluginInstances.count(row.instanceId)) {
                auto& pluginPtr = deviceState->pluginInstances[row.instanceId];
                pluginStatus = pluginPtr.statusMessage;
                pluginInstantiating = pluginPtr.instantiating;
                pluginHasError = pluginPtr.hasError;
            }
        }

        const bool uiSupported = sequencer && row.instanceId >= 0 && sequencer->getPluginInstance(row.instanceId)->hasUISupport();
        const bool uiVisible = sequencer && row.instanceId >= 0 && sequencer->getPluginInstance(row.instanceId)->isUIVisible();

        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(row.deviceLabel.c_str());

        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(row.pluginName.empty() ? "(pending)" : row.pluginName.c_str());

        ImGui::TableSetColumnIndex(2);
        ImGui::TextUnformatted(row.pluginFormat.c_str());

        ImGui::TableSetColumnIndex(3);
        if (pluginInstantiating) {
            ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.3f, 1.0f), "%s", pluginStatus.c_str());
        } else if (pluginHasError || deviceHasError) {
            ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1.0f), "%s", pluginStatus.c_str());
        } else {
            ImGui::TextUnformatted(pluginStatus.c_str());
        }

        ImGui::TableSetColumnIndex(4);
        bool removeTriggered = false;

        std::string menuButtonId = std::format("Actions##menu{}::{}", row.deviceIndex, row.instanceId);
        std::string popupId = std::format("ActionsPopup##{}::{}", row.deviceIndex, row.instanceId);

        if (ImGui::Button(menuButtonId.c_str())) {
            ImGui::OpenPopup(popupId.c_str());
        }

        if (ImGui::BeginPopup(popupId.c_str())) {
            const char* uiText = uiVisible ? "Hide UI" : "Show UI";

            // Show/Hide UI menu item
            if (!uiSupported) {
                ImGui::BeginDisabled();
            }

            if (ImGui::MenuItem(uiText)) {
                if (uiSupported && uiVisible) {
                    sequencer->getPluginInstance(row.instanceId)->hideUI();
                } else if (uiSupported) {
                    sequencer->getPluginInstance(row.instanceId)->showUI();
                }
            }

            if (!uiSupported) {
                ImGui::EndDisabled();
            }

            // Start/Stop device menu item
            if (deviceState && row.instanceId >= 0) {
                const char* runText = deviceRunning ? "Stop" : "Start";
                if (deviceInstantiating) {
                    ImGui::BeginDisabled();
                }

                if (ImGui::MenuItem(runText)) {
                    std::lock_guard guard(deviceState->mutex);
                    auto device = deviceState->device;
                    if (device) {
                        if (deviceState->running) {
                            device->stop();
                            deviceState->running = false;
                            deviceState->statusMessage = "Stopped";
                        } else {
                            auto statusCode = device->start();
                            if (statusCode == 0) {
                                deviceState->running = true;
                                deviceState->statusMessage = "Running";
                                deviceState->hasError = false;
                            } else {
                                deviceState->statusMessage = std::format("Start failed (status {})", statusCode);
                                deviceState->hasError = true;
                            }
                        }
                    }
                }

                if (deviceInstantiating) {
                    ImGui::EndDisabled();
                }
            }

            ImGui::Separator();

            // Remove menu item
            if (ImGui::MenuItem("Remove")) {
                if (row.deviceState && row.instanceId >= 0 && sequencer) {
                    sequencer->getPluginInstance(row.instanceId)->hideUI();
                }
                removeIndex = row.deviceIndex;
                removeTriggered = true;
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }

        return removeTriggered;
    };

    // Render tracks (copied from uapmd-service MainWindow.cpp:596-674)
    if (sequencer) {
        auto trackInfos = sequencer->getTrackInfos();
        bool exitLoops = false;
        for (const auto& track : trackInfos) {
            auto it = rowsByTrack.find(track.trackIndex);
            if (it == rowsByTrack.end()) {
                continue;
            }

            auto& rows = it->second;
            if (rows.empty()) {
                rowsByTrack.erase(it);
                continue;
            }

            std::unordered_map<int32_t, size_t> indexByInstance;
            indexByInstance.reserve(rows.size());
            for (size_t i = 0; i < rows.size(); ++i) {
                indexByInstance[rows[i].instanceId] = i;
            }
            std::vector<bool> displayed(rows.size(), false);

            bool tableOpen = false;

            auto beginTableIfNeeded = [&]() {
                if (!tableOpen) {
                    ImGui::Separator();
                    ImGui::Text("Track %d", track.trackIndex + 1);
                    ImGui::BeginTable(std::format("TrackPlugins-{}-{}", track.trackIndex, track.trackIndex).c_str(), 5,
                                      ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchSame);
                    ImGui::TableSetupColumn("Device");
                    ImGui::TableSetupColumn("Plugin");
                    ImGui::TableSetupColumn("Format", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                    ImGui::TableSetupColumn("Status");
                    ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                    ImGui::TableHeadersRow();
                    tableOpen = true;
                }
            };

            for (const auto& node : track.nodes) {
                auto idxIt = indexByInstance.find(node.instanceId);
                if (idxIt == indexByInstance.end()) {
                    continue;
                }
                beginTableIfNeeded();
                auto& rowRef = rows[idxIt->second];
                displayed[idxIt->second] = true;
                if (renderRow(rowRef)) {
                    exitLoops = true;
                    break;
                }
            }

            if (!exitLoops) {
                for (size_t i = 0; i < rows.size(); ++i) {
                    if (displayed[i]) {
                        continue;
                    }
                    beginTableIfNeeded();
                    if (renderRow(rows[i])) {
                        exitLoops = true;
                        break;
                    }
                }
            }

            if (tableOpen) {
                ImGui::EndTable();
            }

            rowsByTrack.erase(it);

            if (exitLoops) {
                break;
            }
        }
    }

    // Collect remaining rows as pending
    for (auto& [_, rows] : rowsByTrack) {
        pendingRows.insert(pendingRows.end(), rows.begin(), rows.end());
    }
    rowsByTrack.clear();

    // Render pending rows (copied from uapmd-service MainWindow.cpp:681-700)
    if (!pendingRows.empty() && removeIndex == static_cast<size_t>(-1)) {
        ImGui::Separator();
        ImGui::TextUnformatted("Pending plugins");
        if (ImGui::BeginTable("PendingPlugins", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableSetupColumn("Device");
            ImGui::TableSetupColumn("Plugin");
            ImGui::TableSetupColumn("Format", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("Status");
            ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 240.0f);
            ImGui::TableHeadersRow();

            for (auto& row : pendingRows) {
                if (renderRow(row)) {
                    break;
                }
            }

            ImGui::EndTable();
        }
    }

    // Handle removal (copied from uapmd-service MainWindow.cpp:702-704 + removeDevice logic)
    if (removeIndex != static_cast<size_t>(-1)) {
        std::shared_ptr<DeviceState> state;
        {
            std::lock_guard lock(devicesMutex_);
            if (removeIndex < devices_.size()) {
                state = devices_[removeIndex].state;
                devices_.erase(devices_.begin() + static_cast<long>(removeIndex));
            }
        }

        if (state) {
            std::lock_guard guard(state->mutex);
            auto device = state->device;
            for (auto& [instanceId, pluginState] : state->pluginInstances) {
                if (sequencer) {
                    sequencer->getPluginInstance(instanceId)->destroyUI();
                }
            }
            if (device) {
                device->stop();
                deviceController->removeDevice(device->instanceId());
                state->device.reset();
            }
        }

        refreshInstances();
    }
}

}
