#include "MainWindow.hpp"
#include "../AppModel.hpp"
#include <remidy-gui/ContainerWindow.hpp>
#include <imgui.h>
#include <iostream>
#include <algorithm>
#include <format>

namespace uapmd::gui {
MainWindow::MainWindow() {
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

    // Setup MIDI keyboard to show 4 octaves
    midiKeyboard_.setOctaveRange(3, 4); // C3 to C7

    // Setup MIDI keyboard callback
    midiKeyboard_.setKeyEventCallback([this](int note, int velocity, bool isPressed) {
        if (selectedInstance_ >= 0 && selectedInstance_ < static_cast<int>(instances_.size())) {
            auto& sequencer = uapmd::AppModel::instance().sequencer();
            // For now, use the selectedInstance_ as trackIndex
            // This assumes each instance is on its own track
            int32_t trackIndex = selectedInstance_;

            if (isPressed) {
                // Send MIDI note on
                sequencer.sendNoteOn(trackIndex, note);
            } else {
                // Send MIDI note off
                sequencer.sendNoteOff(trackIndex, note);
            }
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

    if (!sequencer.resizePluginUI(instanceId, width, height)) {
        uint32_t adjustedWidth = width;
        uint32_t adjustedHeight = height;
        if (sequencer.getPluginUISize(instanceId, adjustedWidth, adjustedHeight)) {
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

    if (sequencer.resizePluginUI(instanceId, currentWidth, currentHeight))
        return;

    uint32_t adjustedWidth = currentWidth;
    uint32_t adjustedHeight = currentHeight;
    if (!sequencer.getPluginUISize(instanceId, adjustedWidth, adjustedHeight))
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
    sequencer.hidePluginUI(instanceId);
}

bool MainWindow::fetchPluginUISize(int32_t instanceId, uint32_t &width, uint32_t &height) {
    auto& sequencer = uapmd::AppModel::instance().sequencer();
    if (!sequencer.hasPluginUI(instanceId))
        return false;
    return sequencer.getPluginUISize(instanceId, width, height);
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

    // Play/Pause button
    const char* playButtonText = isPlaying_ ? "Pause" : "Play";
    if (ImGui::Button(playButtonText)) {
        playPause();
    }

    ImGui::SameLine();
    if (ImGui::Button("Stop")) {
        stop();
    }

    ImGui::SameLine();
    const char* recordButtonText = isRecording_ ? "Stop Recording" : "Record";
    if (ImGui::Button(recordButtonText)) {
        record();
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
            sequencer.destroyPluginUI(id);
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
    if (ImGui::BeginListBox("##InstanceList", ImVec2(-1, 100))) {
        for (size_t i = 0; i < instances_.size(); i++) {
            const bool isSelected = (selectedInstance_ == static_cast<int>(i));
            int32_t instanceId = instances_[i];
            std::string pluginName = sequencer.getPluginName(instanceId);
            std::string pluginFormat = sequencer.getPluginFormat(instanceId);
            std::string label = pluginName + " (" + pluginFormat + ") (ID: " + std::to_string(instanceId) + ")";
            if (ImGui::Selectable(label.c_str(), isSelected)) {
                selectedInstance_ = static_cast<int>(i);
                refreshParameters();
                refreshPresets();
            }
            if (isSelected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndListBox();
    }

    ImGui::Separator();
    ImGui::Text("MIDI Keyboard:");
    midiKeyboard_.render();
    ImGui::Separator();

    if (selectedInstance_ >= 0 && selectedInstance_ < static_cast<int>(instances_.size())) {
        int32_t instanceId = instances_[selectedInstance_];
        if (sequencer.hasPluginUI(instanceId)) {
            bool isVisible = sequencer.isPluginUIVisible(instanceId);
            auto windowIt = pluginWindows_.find(instanceId);
            const char* uiButtonText = isVisible ? "Hide UI" : "Show UI";
            if (ImGui::Button(uiButtonText)) {
                if (isVisible) {
                    sequencer.hidePluginUI(instanceId);
                    if (windowIt != pluginWindows_.end()) windowIt->second->show(false);
                } else {
                    // Create container window if needed
                    windowIt = pluginWindows_.find(instanceId);
                    remidy::gui::ContainerWindow* container = nullptr;
                    if (windowIt == pluginWindows_.end()) {
                        std::string windowTitle = sequencer.getPluginName(instanceId) + " (" + sequencer.getPluginFormat(instanceId) + ")";
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
                        return;
                    }

                    container->show(true);
                    void* parentHandle = container->getHandle();

                    // Check if plugin UI has been created (pluginWindowEmbedded_ tracks this)
                    bool pluginUIExists = (pluginWindowEmbedded_.find(instanceId) != pluginWindowEmbedded_.end());

                    if (!pluginUIExists) {
                        // First time: create plugin UI with resize handler
                        if (!sequencer.createPluginUI(instanceId, false, parentHandle,
                            [this, instanceId](uint32_t w, uint32_t h){ return handlePluginResizeRequest(instanceId, w, h); })) {
                            container->show(false);
                            pluginWindows_.erase(instanceId);
                            std::cout << "Failed to create plugin UI for instance " << instanceId << std::endl;
                            return;
                        }
                        pluginWindowEmbedded_[instanceId] = true;
                    }

                    // Show the plugin UI (whether just created or already exists)
                    if (!sequencer.showPluginUI(instanceId, false, parentHandle)) {
                        std::cout << "Failed to show plugin UI for instance " << instanceId << std::endl;
                    }
                }
            }
            // Presets/parameters UI continues below
        }
    }

    // Preset management
    ImGui::Text("Presets:");
    if (ImGui::BeginCombo("##PresetCombo", selectedPreset_ >= 0 ? presets_[selectedPreset_].name.c_str() : "Select preset...")) {
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

    // No embedded panel; container windows are separate native windows

    // Parameters - in a scrollable region
    ImGui::Text("Parameters:");
    if (ImGui::BeginChild("ParametersChild", ImVec2(0, 200), true, ImGuiWindowFlags_HorizontalScrollbar)) {
        renderParameterControls();
    }
    ImGui::EndChild();
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

void MainWindow::playPause() {
    auto& sequencer = uapmd::AppModel::instance().sequencer();

    if (isPlaying_) {
        sequencer.stopAudio();
        isPlaying_ = false;
        std::cout << "Stopping playback" << std::endl;
    } else {
        sequencer.startAudio();
        isPlaying_ = true;
        std::cout << "Starting playback" << std::endl;
    }
}

void MainWindow::stop() {
    auto& sequencer = uapmd::AppModel::instance().sequencer();
    sequencer.stopAudio();

    isPlaying_ = false;
    playbackPosition_ = 0.0f;

    std::cout << "Stopping playback" << std::endl;
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
        presets_.clear();
    }
}

void MainWindow::refreshParameters() {
    parameters_.clear();
    parameterValues_.clear();

    if (selectedInstance_ < 0 || selectedInstance_ >= static_cast<int>(instances_.size())) {
        return;
    }

    int32_t instanceId = instances_[selectedInstance_];
    parameters_ = uapmd::AppModel::instance().sequencer().getParameterList(instanceId);

    // Initialize parameter values with their initial values
    parameterValues_.resize(parameters_.size());
    for (size_t i = 0; i < parameters_.size(); ++i) {
        parameterValues_[i] = static_cast<float>(parameters_[i].initialValue);
    }
}

void MainWindow::refreshPresets() {
    presets_.clear();

    if (selectedInstance_ < 0 || selectedInstance_ >= static_cast<int>(instances_.size())) {
        return;
    }

    int32_t instanceId = instances_[selectedInstance_];
    presets_ = uapmd::AppModel::instance().sequencer().getPresetList(instanceId);
    selectedPreset_ = -1;
}

void MainWindow::loadSelectedPreset() {
    if (selectedInstance_ < 0 || selectedInstance_ >= static_cast<int>(instances_.size()) ||
        selectedPreset_ < 0 || selectedPreset_ >= static_cast<int>(presets_.size())) {
        return;
        }

    int32_t instanceId = instances_[selectedInstance_];
    int32_t presetIndex = presets_[selectedPreset_].index;

    uapmd::AppModel::instance().sequencer().loadPreset(instanceId, presetIndex);

    std::cout << "Loading preset " << presets_[selectedPreset_].name
              << " for instance " << instanceId << std::endl;

    // Refresh parameters after preset load
    refreshParameters();
}

void MainWindow::renderParameterControls() {
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

            if (param.hidden || !param.automatable)
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
            std::string sliderId = "##" + std::to_string(param.index);
            if (ImGui::SliderFloat(sliderId.c_str(), &parameterValues_[i], static_cast<float>(param.minValue), static_cast<float>(param.maxValue))) {
                int32_t instanceId = instances_[selectedInstance_];
                uapmd::AppModel::instance().sequencer().setParameterValue(instanceId, param.index, parameterValues_[i]);
                std::cout << "Parameter " << param.name << " changed to " << parameterValues_[i] << std::endl;
            }

            ImGui::TableNextColumn();
            std::string resetId = "Reset##" + std::to_string(param.index);
            if (ImGui::Button(resetId.c_str())) {
                parameterValues_[i] = static_cast<float>(param.initialValue);
                int32_t instanceId = instances_[selectedInstance_];
                uapmd::AppModel::instance().sequencer().setParameterValue(instanceId, param.index, parameterValues_[i]);
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

}
