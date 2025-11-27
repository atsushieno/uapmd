#include "MainWindow.hpp"
#include "../AppModel.hpp"
#include <remidy-gui/remidy-gui.hpp>
#include <remidy/priv/common.hpp>
#include <remidy/priv/plugin-parameter.hpp>
#include <imgui.h>
#include <iostream>
#include <algorithm>
#include <format>
#include <limits>
#include <fstream>
#include <portable-file-dialogs.h>

#include "SharedTheme.hpp"

namespace {
std::string formatPlainValueLabel(double value) {
    return std::format("{:.7g}", value);
}
}

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
    if (ImGui::BeginTable("##InstanceTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableSetupColumn("Plugin", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Format", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 300.0f);
        ImGui::TableHeadersRow();

        for (size_t i = 0; i < instances_.size(); i++) {
            int32_t instanceId = instances_[i];
            std::string pluginName = sequencer.getPluginName(instanceId);
            std::string pluginFormat = sequencer.getPluginFormat(instanceId);
            auto* instance = sequencer.getPluginInstance(instanceId);

            ImGui::TableNextRow();

            // Plugin name column
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%s", pluginName.c_str());

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
            ImGui::SameLine();

            // Details button
            auto detailsIt = detailsWindows_.find(instanceId);
            bool detailsVisible = (detailsIt != detailsWindows_.end() && detailsIt->second.visible);
            const char* detailsButtonText = detailsVisible ? "Hide Details" : "Show Details";
            std::string detailsButtonId = std::string(detailsButtonText) + "##details" + std::to_string(instanceId);

            if (ImGui::Button(detailsButtonId.c_str())) {
                if (detailsVisible) {
                    hideDetailsWindow(instanceId);
                } else {
                    showDetailsWindow(instanceId);
                }
            }

            // line break

            // Save button
            std::string saveButtonId = "Save##save" + std::to_string(instanceId);
            if (ImGui::Button(saveButtonId.c_str())) {
                savePluginState(instanceId);
            }
            ImGui::SameLine();

            // Load button
            std::string loadButtonId = "Load##load" + std::to_string(instanceId);
            if (ImGui::Button(loadButtonId.c_str())) {
                loadPluginState(instanceId);
            }
            ImGui::SameLine();

            // Remove button
            std::string removeButtonId = "Remove##remove" + std::to_string(instanceId);
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

    if (ImGui::BeginTable("ParameterTable", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Index", ImGuiTableColumnFlags_WidthFixed, 30.0f);
        ImGui::TableSetupColumn("Stable ID", ImGuiTableColumnFlags_WidthFixed, 50.0f);
        ImGui::TableSetupColumn("Parameter", ImGuiTableColumnFlags_WidthStretch, 0.0f, 0);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 180.0f);
        ImGui::TableSetupColumn("Default", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableHeadersRow();

        std::string filter = state.parameterFilter;
        std::transform(filter.begin(), filter.end(), filter.begin(), ::tolower);

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
            const char* format = state.parameterValueStrings[i].empty()
                                     ? "%.3f"
                                     : state.parameterValueStrings[i].c_str();

            float sliderWidth = ImGui::GetContentRegionAvail().x;
            float comboButtonWidth = 0.0f;
            float comboSpacing = 0.0f;
            if (hasDiscreteCombo) {
                comboButtonWidth = ImGui::GetFrameHeight();
                comboSpacing = ImGui::GetStyle().ItemInnerSpacing.x;
                sliderWidth = std::max(20.0f, sliderWidth - (comboButtonWidth + comboSpacing));
            }

            ImGui::SetNextItemWidth(sliderWidth);
            if (ImGui::SliderFloat(controlId.c_str(), &state.parameterValues[i],
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
                        const std::string& currentLabel = state.parameterValueStrings[i].empty()
                                                               ? std::to_string(state.parameterValues[i])
                                                               : state.parameterValueStrings[i];

                        if (!currentLabel.empty()) {
                            ImGui::TextUnformatted(currentLabel.c_str());
                            ImGui::Separator();
                        }

                        for (const auto& namedValue : param.namedValues) {
                            bool isSelected = (std::abs(namedValue.value - state.parameterValues[i]) < 0.0001);
                            if (ImGui::Selectable(namedValue.name.c_str(), isSelected)) {
                                state.parameterValues[i] = static_cast<float>(namedValue.value);
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
                sequencer.setParameterValue(instanceId, param.index, state.parameterValues[i]);
                updateParameterValueString(i, instanceId, state);
                std::cout << "Parameter " << param.name << " changed to " << state.parameterValues[i] << std::endl;
            }

            ImGui::TableNextColumn();
            std::string resetId = "Reset##" + std::to_string(param.index);
            if (ImGui::Button(resetId.c_str())) {
                state.parameterValues[i] = static_cast<float>(param.defaultPlainValue);
                sequencer.setParameterValue(instanceId, param.index, state.parameterValues[i]);
                updateParameterValueString(i, instanceId, state);
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
                const char* presetPreview =
                    (detailsState.selectedPreset >= 0 &&
                     detailsState.selectedPreset < static_cast<int>(detailsState.presets.size()))
                        ? detailsState.presets[detailsState.selectedPreset].name.c_str()
                        : "Select preset...";
                if (ImGui::BeginCombo("##PresetCombo", presetPreview)) {
                    for (size_t i = 0; i < detailsState.presets.size(); i++) {
                        const bool isSelected = (detailsState.selectedPreset == static_cast<int>(i));
                        if (ImGui::Selectable(detailsState.presets[i].name.c_str(), isSelected)) {
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

}
