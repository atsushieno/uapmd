#include "MainWindow.hpp"
#include "../AppModel.hpp"
#include <remidy-gui/remidy-gui.hpp>
#include <imgui.h>
#include <iostream>
#include <algorithm>
#include <format>
#include <limits>

#include "SharedTheme.hpp"

namespace uapmd::gui {
MainWindow::MainWindow() {
    SetupImGuiStyle();

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

                // If we just instantiated the first plugin, select it
                if (instances_.size() == 1) {
                    selectedInstance_ = 0;
                    refreshParameters();
                    refreshPresets();
                }
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

    if (!recentFiles_.empty()) {
        ImGui::SameLine();
        if (ImGui::BeginCombo("Recent Files", "Recent...")) {
            for (const auto& file : recentFiles_) {
                if (ImGui::Selectable(file.c_str())) {
                    currentFile_ = file;
                    std::cout << "Loading file: " << currentFile_ << std::endl;
                }
            }
            ImGui::EndCombo();
        }
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
        sequencer.setOfflineRendering(offlineRendering);
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

    ImGui::SameLine();

    // Remove instance button - only enabled if an instance is selected
    bool canRemove = selectedInstance_ >= 0 && selectedInstance_ < static_cast<int>(instances_.size());
    if (!canRemove) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Remove Instance")) {
        int32_t instanceId = instances_[selectedInstance_];

        // Hide and cleanup UI if it's open
        auto* instance = sequencer.getPluginInstance(instanceId);
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
    }
    if (!canRemove) {
        ImGui::EndDisabled();
    }

    ImGui::Text("Active Instances:");
    if (ImGui::BeginTable("##InstanceTable", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableSetupColumn("Plugin", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Format", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Plugin UI", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Details", ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableHeadersRow();

        for (size_t i = 0; i < instances_.size(); i++) {
            int32_t instanceId = instances_[i];
            std::string pluginName = sequencer.getPluginName(instanceId);
            std::string pluginFormat = sequencer.getPluginFormat(instanceId);
            auto* instance = sequencer.getPluginInstance(instanceId);

            ImGui::TableNextRow();

            // Plugin name column (selectable)
            ImGui::TableSetColumnIndex(0);
            const bool isSelected = (selectedInstance_ == static_cast<int>(i));
            if (ImGui::Selectable(pluginName.c_str(), isSelected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
                selectedInstance_ = static_cast<int>(i);
                refreshParameters();
                refreshPresets();
            }

            // Format column
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%s", pluginFormat.c_str());

            // Plugin UI button column
            ImGui::TableSetColumnIndex(2);
            bool hasUI = instance->hasUISupport();
            bool isVisible = instance->isUIVisible();
            const char* uiButtonText = isVisible ? "Hide UI" : "Show UI";
            std::string uiButtonId = std::string(uiButtonText) + "##ui" + std::to_string(instanceId);

            if (!hasUI) {
                ImGui::BeginDisabled();
            }

            if (ImGui::Button(uiButtonId.c_str())) {
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

            // Details button column
            ImGui::TableSetColumnIndex(3);
            auto detailsIt = detailsWindows_.find(instanceId);
            bool detailsVisible = (detailsIt != detailsWindows_.end() && detailsIt->second.visible);
            const char* detailsButtonText = detailsVisible ? "Hide Details" : "Show Details";
            std::string detailsButtonId = std::string(detailsButtonText) + "##det" + std::to_string(instanceId);

            if (ImGui::Button(detailsButtonId.c_str())) {
                if (detailsVisible) {
                    hideDetailsWindow(instanceId);
                } else {
                    showDetailsWindow(instanceId);
                }
            }

            // Remove button column
            ImGui::TableSetColumnIndex(4);
            std::string removeButtonId = "Remove##" + std::to_string(instanceId);
            if (ImGui::Button(removeButtonId.c_str())) {
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
                break; // Exit loop after removal since we're modifying the list
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
    currentFile_ = "loaded_file.wav";
    playbackLength_ = 120.0f; // 2 minutes
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

    // Reset selection if out of bounds
    if (selectedInstance_ >= static_cast<int>(instances_.size())) {
        selectedInstance_ = -1;
        parameters_.clear();
        parameterValues_.clear();
        parameterValueStrings_.clear();
        presets_.clear();
    }
}

void MainWindow::refreshParameters() {
    parameters_.clear();
    parameterValues_.clear();
    parameterValueStrings_.clear();

    if (selectedInstance_ < 0 || selectedInstance_ >= static_cast<int>(instances_.size())) {
        return;
    }

    int32_t instanceId = instances_[selectedInstance_];
    auto* pal = uapmd::AppModel::instance().sequencer().getPluginInstance(instanceId);
    parameters_ = pal->parameterMetadataList();

    // Initialize parameter values with their initial values
    parameterValues_.resize(parameters_.size());
    parameterValueStrings_.resize(parameters_.size());
    for (size_t i = 0; i < parameters_.size(); ++i) {
        parameterValues_[i] = static_cast<float>(parameters_[i].defaultPlainValue);
        updateParameterValueString(i);
    }
}

void MainWindow::refreshPresets() {
    presets_.clear();

    if (selectedInstance_ < 0 || selectedInstance_ >= static_cast<int>(instances_.size())) {
        return;
    }

    int32_t instanceId = instances_[selectedInstance_];
    auto* pal = uapmd::AppModel::instance().sequencer().getPluginInstance(instanceId);
    presets_ = pal->presetMetadataList();
    selectedPreset_ = -1;
}

void MainWindow::loadSelectedPreset() {
    if (selectedInstance_ < 0 || selectedInstance_ >= static_cast<int>(instances_.size()) ||
        selectedPreset_ < 0 || selectedPreset_ >= static_cast<int>(presets_.size())) {
        return;
        }

    int32_t instanceId = instances_[selectedInstance_];
    int32_t presetIndex = presets_[selectedPreset_].index;

    auto* pal = uapmd::AppModel::instance().sequencer().getPluginInstance(instanceId);
    pal->loadPreset(presetIndex);

    std::cout << "Loading preset " << presets_[selectedPreset_].name
              << " for instance " << instanceId << std::endl;

    // Refresh parameters after preset load
    refreshParameters();
}

void MainWindow::updateParameterValueString(size_t parameterIndex) {
    if (parameterIndex >= parameters_.size() ||
        selectedInstance_ < 0 || selectedInstance_ >= static_cast<int>(instances_.size())) {
        return;
    }

    int32_t instanceId = instances_[selectedInstance_];
    auto& param = parameters_[parameterIndex];
    auto* pal = uapmd::AppModel::instance().sequencer().getPluginInstance(instanceId);
    parameterValueStrings_[parameterIndex] = pal->getParameterValueString(
        param.index, parameterValues_[parameterIndex]);
}

void MainWindow::renderParameterControls() {
    // Reflect Event Out toggle
    ImGui::Checkbox("Reflect Event Out", &reflectEventOut_);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("When enabled, parameter changes from plugin output events\nwill be reflected in the UI controls");
    }

    // Check for parameter updates from plugin output
    if (reflectEventOut_ && selectedInstance_ >= 0 && selectedInstance_ < static_cast<int>(instances_.size())) {
        int32_t instanceId = instances_[selectedInstance_];
        auto& sequencer = uapmd::AppModel::instance().sequencer();
        auto updates = sequencer.getParameterUpdates(instanceId);

        for (const auto& update : updates) {
            // Find the parameter by index and update its value
            for (size_t i = 0; i < parameters_.size(); ++i) {
                if (parameters_[i].index == update.parameterIndex) {
                    parameterValues_[i] = static_cast<float>(update.value);
                    updateParameterValueString(i);
                    break;
                }
            }
        }
    }

    ImGui::InputText("Filter Parameters", parameterFilter_, sizeof(parameterFilter_));

    if (ImGui::BeginTable("ParameterTable", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Index", ImGuiTableColumnFlags_WidthFixed, 30.0f);
        ImGui::TableSetupColumn("Stable ID", ImGuiTableColumnFlags_WidthFixed, 50.0f);
        ImGui::TableSetupColumn("Parameter", ImGuiTableColumnFlags_WidthStretch, 0.0f, 0);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 180.0f);
        ImGui::TableSetupColumn("Default", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableHeadersRow();

        std::string filter = parameterFilter_;
        std::transform(filter.begin(), filter.end(), filter.begin(), ::tolower);

        for (size_t i = 0; i < parameters_.size(); ++i) {
            auto& param = parameters_[i];

            if (param.hidden || (!param.automatable && !param.discrete))
                continue;

            // Filter parameters by name or stable ID
            if (!filter.empty()) {
                std::string name = param.name;
                std::string stableId = param.stableId;
                std::transform(name.begin(), name.end(), name.begin(), ::tolower);
                std::transform(stableId.begin(), stableId.end(), stableId.begin(), ::tolower);
                if (name.find(filter) == std::string::npos && stableId.find(filter) == std::string::npos) {
                    continue;
                }
            }

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
            int32_t instanceId = instances_[selectedInstance_];

            // Use combobox for discrete parameters with named values
            if (param.discrete && !param.namedValues.empty()) {
                // Use cached value label
                const std::string& currentLabel = parameterValueStrings_[i].empty()
                    ? std::to_string(parameterValues_[i])
                    : parameterValueStrings_[i];

                if (ImGui::BeginCombo(controlId.c_str(), currentLabel.c_str())) {
                    for (const auto& namedValue : param.namedValues) {
                        bool isSelected = (std::abs(namedValue.value - parameterValues_[i]) < 0.0001);
                        if (ImGui::Selectable(namedValue.name.c_str(), isSelected)) {
                            parameterValues_[i] = static_cast<float>(namedValue.value);
                            parameterChanged = true;
                        }
                        if (isSelected) {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::EndCombo();
                }
            } else {
                // Use slider for continuous parameters
                // Use cached value label
                const char* format = parameterValueStrings_[i].empty() ? "%.3f" : parameterValueStrings_[i].c_str();
                if (ImGui::SliderFloat(controlId.c_str(), &parameterValues_[i], static_cast<float>(param.minPlainValue), static_cast<float>(param.maxPlainValue), format)) {
                    parameterChanged = true;
                }
            }

            if (parameterChanged) {
                uapmd::AppModel::instance().sequencer().setParameterValue(instanceId, param.index, parameterValues_[i]);
                // Update cached string after value change
                updateParameterValueString(i);
                std::cout << "Parameter " << param.name << " changed to " << parameterValues_[i] << std::endl;
            }

            ImGui::TableNextColumn();
            std::string resetId = "Reset##" + std::to_string(param.index);
            if (ImGui::Button(resetId.c_str())) {
                parameterValues_[i] = static_cast<float>(param.defaultPlainValue);
                int32_t instanceId = instances_[selectedInstance_];
                uapmd::AppModel::instance().sequencer().setParameterValue(instanceId, param.index, parameterValues_[i]);
                // Update cached string after reset
                updateParameterValueString(i);
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

            std::string selectableId = plugin.format + "##" + plugin.id + "##" + std::to_string(sortedIndex);
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

    // Plugin instantiation controls
    bool canInstantiate = !selectedPluginFormat_.empty() && !selectedPluginId_.empty();
    if (!canInstantiate) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Instantiate Plugin")) {
        static int instancingIdCounter = 1;
        int instancingId = instancingIdCounter++;

        uapmd::AppModel::instance().instantiatePlugin(instancingId, selectedPluginFormat_, selectedPluginId_);
        std::cout << "Instantiating plugin: " << selectedPluginFormat_ << " - " << selectedPluginId_
                  << " with ID " << instancingId << std::endl;
        showPluginSelector_ = false;
    }
    if (!canInstantiate) {
        ImGui::EndDisabled();
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
            // Find this instance in the instances list
            auto instanceIt = std::find(instances_.begin(), instances_.end(), instanceId);
            if (instanceIt != instances_.end()) {
                int instanceIndex = static_cast<int>(instanceIt - instances_.begin());

                // Store previous selection to restore later
                int prevSelected = selectedInstance_;
                bool needsRestore = (selectedInstance_ != instanceIndex);

                // Temporarily switch to this instance if needed
                if (needsRestore) {
                    selectedInstance_ = instanceIndex;
                }

                // MIDI Keyboard section
                ImGui::Text("MIDI Keyboard:");
                detailsState.midiKeyboard.render();
                ImGui::Separator();

                // Presets section (moved before parameters)
                ImGui::Text("Presets:");
                if (ImGui::BeginCombo("##PresetCombo", selectedPreset_ >= 0 && selectedPreset_ < static_cast<int>(presets_.size()) ? presets_[selectedPreset_].name.c_str() : "Select preset...")) {
                    for (size_t i = 0; i < presets_.size(); i++) {
                        const bool isSelected = (selectedPreset_ == static_cast<int>(i));
                        if (ImGui::Selectable(presets_[i].name.c_str(), isSelected)) {
                            selectedPreset_ = static_cast<int>(i);
                        }
                        if (isSelected) {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::EndCombo();
                }

                ImGui::SameLine();
                bool canLoadPreset = selectedPreset_ >= 0 && selectedPreset_ < static_cast<int>(presets_.size());
                if (!canLoadPreset) {
                    ImGui::BeginDisabled();
                }
                if (ImGui::Button("Load Preset")) {
                    loadSelectedPreset();
                }
                if (!canLoadPreset) {
                    ImGui::EndDisabled();
                }

                ImGui::Separator();

                // Parameters section - expands to fill remaining space
                ImGui::Text("Parameters:");
                if (ImGui::BeginChild("ParametersChild", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar)) {
                    renderParameterControls();
                }
                ImGui::EndChild();

                // Restore previous selection
                if (needsRestore) {
                    selectedInstance_ = prevSelected;
                }
            }
        }
        ImGui::End();

        // Update visibility if user closed the window
        if (!windowOpen) {
            hideDetailsWindow(instanceId);
        }
    }
}

}
