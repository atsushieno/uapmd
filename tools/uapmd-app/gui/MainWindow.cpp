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
#include <cmath>
#include <portable-file-dialogs.h>

#include <midicci/midicci.hpp> // include before anything that indirectly includes X.h
#include <cmidi2.h>

#include <imgui.h>

#include "SharedTheme.hpp"

#include "MainWindow.hpp"
#include "../AppModel.hpp"
#include "uapmd/priv/audio/AudioFileFactory.hpp"

namespace {
using ParameterContext = uapmd::gui::ParameterList::ParameterContext;

struct PerNoteSelection {
    remidy::PerNoteControllerContextTypes type{remidy::PerNoteControllerContextTypes::PER_NOTE_CONTROLLER_NONE};
    remidy::PerNoteControllerContext context{};
};

std::optional<PerNoteSelection> buildPerNoteSelection(const uapmd::gui::ParameterList& list) {
    PerNoteSelection selection{};
    selection.context = remidy::PerNoteControllerContext{};
    switch (list.context()) {
    case ParameterContext::Global:
        return std::nullopt;
    case ParameterContext::Group:
        selection.type = remidy::PerNoteControllerContextTypes::PER_NOTE_CONTROLLER_PER_GROUP;
        selection.context.group = list.contextValue();
        break;
    case ParameterContext::Channel:
        selection.type = remidy::PerNoteControllerContextTypes::PER_NOTE_CONTROLLER_PER_CHANNEL;
        selection.context.channel = list.contextValue();
        break;
    case ParameterContext::Key:
        selection.type = remidy::PerNoteControllerContextTypes::PER_NOTE_CONTROLLER_PER_NOTE;
        selection.context.note = list.contextValue();
        break;
    }
    return selection;
}

std::vector<uapmd::ParameterMetadata> toParameterMetadata(const std::vector<remidy::PluginParameter*>& pluginParams) {
    std::vector<uapmd::ParameterMetadata> metadata;
    metadata.reserve(pluginParams.size());
    for (auto* param : pluginParams) {
        if (!param) {
            continue;
        }
        std::vector<uapmd::ParameterNamedValue> namedValues;
        namedValues.reserve(param->enums().size());
        for (const auto& enumeration : param->enums()) {
            namedValues.push_back(uapmd::ParameterNamedValue{
                .value = enumeration.value,
                .name = enumeration.label
            });
        }
        metadata.push_back(uapmd::ParameterMetadata{
            .index = param->index(),
            .stableId = param->stableId(),
            .name = param->name(),
            .path = param->path(),
            .defaultPlainValue = param->defaultPlainValue(),
            .minPlainValue = param->minPlainValue(),
            .maxPlainValue = param->maxPlainValue(),
            .automatable = param->automatable(),
            .hidden = param->hidden(),
            .discrete = param->discrete(),
            .namedValues = std::move(namedValues)
        });
    }
    return metadata;
}

}

namespace uapmd::gui {
MainWindow::MainWindow(GuiDefaults defaults) {
    SetupImGuiStyle();
    baseStyle_ = ImGui::GetStyle();
    captureFontScales();
    applyUiScale(uiScale_);

    // Apply defaults from command line arguments
    // TODO: If needed, implement default plugin selection through pluginList_

    refreshDeviceList();
    refreshInstances();
    refreshPluginList();

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

    // Register callback for when plugin instances are created (GUI or script)
    uapmd::AppModel::instance().instanceCreated.push_back(
        [this](const uapmd::AppModel::PluginInstanceResult& result) {
            if (!result.error.empty() || result.instanceId < 0) {
                return;  // Error already logged elsewhere
            }

            // This callback handles UI state updates for ALL instance creations
            auto state = std::make_shared<DeviceState>();
            state->device = result.device;
            state->label = result.device ? std::format("{} [format]", result.pluginName) : "";
            state->apiName = "default";  // FIXME: get from result

            // Start the device
            int startStatus = result.device ? result.device->start() : -1;
            if (startStatus == 0) {
                state->running = true;
                state->statusMessage = "Running";

                auto& node = state->pluginInstances[result.instanceId];
                node.instanceId = result.instanceId;
                node.pluginName = result.pluginName;
                node.pluginFormat = "";  // FIXME: get from result
                node.pluginId = "";
                node.statusMessage = std::format("Plugin ready (instance {})", result.instanceId);
                node.instantiating = false;
                node.hasError = false;
                node.trackIndex = result.trackIndex;
            } else {
                state->running = false;
                state->hasError = true;
                state->statusMessage = std::format("Failed to start device (status {})", startStatus);
            }
            state->instantiating = false;

            // Add to devices list
            {
                std::lock_guard lock(devicesMutex_);
                devices_.push_back(DeviceEntry{nextDeviceId_++, state});
            }

            refreshInstances();
            if (startStatus == 0) {
                // Ensure the UMP device name buffer reflects the device name
                umpDeviceNameBuffers_[result.instanceId] = {};
                std::strncpy(umpDeviceNameBuffers_[result.instanceId].data(), state->label.c_str(),
                             umpDeviceNameBuffers_[result.instanceId].size() - 1);
                umpDeviceNameBuffers_[result.instanceId][umpDeviceNameBuffers_[result.instanceId].size() - 1] = '\0';
            }
        });

    // Register callback for when plugin instances are removed (GUI or script)
    uapmd::AppModel::instance().instanceRemoved.push_back(
        [this](int32_t instanceId) {
            // Remove from devices list
            {
                std::lock_guard lock(devicesMutex_);
                for (auto it = devices_.begin(); it != devices_.end(); ++it) {
                    auto state = it->state;
                    std::lock_guard guard(state->mutex);
                    if (state->pluginInstances.count(instanceId) > 0) {
                        devices_.erase(it);
                        break;
                    }
                }
            }

            // Refresh the instance list
            refreshInstances();
        });

    // Register callback for when devices are enabled
    uapmd::AppModel::instance().deviceEnabled.push_back(
        [this](const uapmd::AppModel::DeviceStateResult& result) {
            // Find and update the device state
            std::lock_guard lock(devicesMutex_);
            for (auto& entry : devices_) {
                auto state = entry.state;
                std::lock_guard guard(state->mutex);
                if (state->pluginInstances.count(result.instanceId) > 0) {
                    state->running = result.running;
                    state->statusMessage = result.statusMessage;
                    state->hasError = !result.success;
                    break;
                }
            }
        });

    // Register callback for when devices are disabled
    uapmd::AppModel::instance().deviceDisabled.push_back(
        [this](const uapmd::AppModel::DeviceStateResult& result) {
            // Find and update the device state
            std::lock_guard lock(devicesMutex_);
            for (auto& entry : devices_) {
                auto state = entry.state;
                std::lock_guard guard(state->mutex);
                if (state->pluginInstances.count(result.instanceId) > 0) {
                    state->running = result.running;
                    state->statusMessage = result.statusMessage;
                    state->hasError = !result.success;
                    break;
                }
            }
        });

    // Set up TrackList callbacks
    trackList_.setOnShowUI([this](int32_t instanceId) {
        handleShowUI(instanceId);
    });

    trackList_.setOnHideUI([this](int32_t instanceId) {
        handleHideUI(instanceId);
    });

    trackList_.setOnShowDetails([this](int32_t instanceId) {
        showDetailsWindow(instanceId);
    });

    trackList_.setOnHideDetails([this](int32_t instanceId) {
        hideDetailsWindow(instanceId);
    });

    trackList_.setOnEnableDevice([this](int32_t instanceId, const std::string& deviceName) {
        handleEnableDevice(instanceId, deviceName);
    });

    trackList_.setOnDisableDevice([this](int32_t instanceId) {
        handleDisableDevice(instanceId);
    });

    trackList_.setOnSaveState([this](int32_t instanceId) {
        savePluginState(instanceId);
    });

    trackList_.setOnLoadState([this](int32_t instanceId) {
        loadPluginState(instanceId);
    });

    trackList_.setOnRemoveInstance([this](int32_t instanceId) {
        handleRemoveInstance(instanceId);
    });

    trackList_.setOnUMPDeviceNameChange([this](int32_t instanceId, const std::string& newName) {
        // Update the UMP device name buffer
        if (umpDeviceNameBuffers_.find(instanceId) != umpDeviceNameBuffers_.end()) {
            std::strncpy(umpDeviceNameBuffers_[instanceId].data(), newName.c_str(),
                        umpDeviceNameBuffers_[instanceId].size() - 1);
            umpDeviceNameBuffers_[instanceId][umpDeviceNameBuffers_[instanceId].size() - 1] = '\0';
        }
    });

    // Set up AudioDeviceSettings callbacks
    audioDeviceSettings_.setOnDeviceChanged([this]() {
        handleAudioDeviceChange();
    });

    // Register device change listener with AudioIODeviceManager
    auto audioManager = uapmd::AudioIODeviceManager::instance();
    audioManager->setDeviceChangeCallback([this](int32_t deviceId, AudioIODeviceChange change) {
        // Refresh device list when devices are added or removed
        refreshDeviceList();
    });
}

void MainWindow::render(void* window) {
    // Use the entire screen space as the main window (no nested window)
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 displaySize = io.DisplaySize;
    constexpr float kWindowSizeEpsilon = 1.0f;
    if (displaySize.x > 0.0f && displaySize.y > 0.0f) {
        if (lastWindowSize_.x == 0.0f && lastWindowSize_.y == 0.0f) {
            baseWindowSize_.x = displaySize.x / uiScale_;
            baseWindowSize_.y = displaySize.y / uiScale_;
        }

        const float deltaX = std::fabs(displaySize.x - lastWindowSize_.x);
        const float deltaY = std::fabs(displaySize.y - lastWindowSize_.y);

        if (waitingForWindowResize_) {
            const bool reachedTarget = std::fabs(displaySize.x - requestedWindowSize_.x) < kWindowSizeEpsilon &&
                                       std::fabs(displaySize.y - requestedWindowSize_.y) < kWindowSizeEpsilon;
            if (reachedTarget || (deltaX > kWindowSizeEpsilon || deltaY > kWindowSizeEpsilon)) {
                waitingForWindowResize_ = false;
                baseWindowSize_.x = displaySize.x / uiScale_;
                baseWindowSize_.y = displaySize.y / uiScale_;
            }
        } else if (deltaX > kWindowSizeEpsilon || deltaY > kWindowSizeEpsilon) {
            baseWindowSize_.x = displaySize.x / uiScale_;
            baseWindowSize_.y = displaySize.y / uiScale_;
        }

        lastWindowSize_ = displaySize;
    }
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(displaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f * uiScale_, 8.0f * uiScale_));

    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

    if (ImGui::Begin("MainAppWindow", nullptr, window_flags)) {
        if (ImGui::BeginChild("MainToolbar", ImVec2(0, 80.0f * uiScale_), false, ImGuiWindowFlags_NoScrollbar)) {
            if (ImGui::Button("Device Settings")) {
                showDeviceSettingsWindow_ = !showDeviceSettingsWindow_;
            }
            ImGui::SameLine();
            if (ImGui::Button("Player Settings")) {
                showPlayerSettingsWindow_ = !showPlayerSettingsWindow_;
            }
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Scale:");
            ImGui::SameLine();
            static constexpr float scaleOptions[] = {0.5f, 0.8f, 1.0f, 1.2f, 1.5f, 2.0f, 4.0f};
            static constexpr const char* scaleLabels[] = {"x0.5", "x0.8", "x1.0", "x1.2", "x1.5", "x2.0", "x4.0"};
            int currentScaleIndex = 0;
            for (size_t i = 0; i < std::size(scaleOptions); ++i) {
                if (std::fabs(uiScale_ - scaleOptions[i]) < 0.001f) {
                    currentScaleIndex = static_cast<int>(i);
                    break;
                }
            }
            int selectedIndex = currentScaleIndex;
            ImGui::SetNextItemWidth(100.0f * uiScale_);
            if (ImGui::BeginCombo("##UiScaleCombo", scaleLabels[currentScaleIndex])) {
                for (int i = 0; i < static_cast<int>(std::size(scaleOptions)); ++i) {
                    bool isSelected = (selectedIndex == i);
                    if (ImGui::Selectable(scaleLabels[i], isSelected)) {
                        selectedIndex = i;
                    }
                    if (isSelected) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
            if (selectedIndex != currentScaleIndex) {
                applyUiScale(scaleOptions[selectedIndex]);
                requestWindowResize();
            }

            ImGui::Dummy(ImVec2(0, 12.0f * uiScale_));

            if (ImGui::Button("Plugins")) {
                showPluginSelectorWindow_ = !showPluginSelectorWindow_;
            }
            ImGui::SameLine();
            if (ImGui::Button("Script")) {
                if (scriptEditor_.isOpen())
                    scriptEditor_.hide();
                else
                    scriptEditor_.show();
            }
        }
        ImGui::EndChild();

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

    // Render floating windows
    renderPluginSelectorWindow();
    renderDeviceSettingsWindow();
    renderPlayerSettingsWindow();
    renderDetailsWindows();
    scriptEditor_.render();

    uiScaleDirty_ = false;
}

void MainWindow::renderDeviceSettingsWindow() {
    if (!showDeviceSettingsWindow_) {
        return;
    }

    const std::string windowId = "DeviceSettings";
    setNextChildWindowSize(windowId, ImVec2(400.0f, 360.0f));
    if (ImGui::Begin("Device Settings", &showDeviceSettingsWindow_)) {
        updateChildWindowSizeState(windowId);
        updateAudioDeviceSettingsData();
        audioDeviceSettings_.render();
    }
    ImGui::End();
}

void MainWindow::renderPlayerSettingsWindow() {
    if (!showPlayerSettingsWindow_) {
        return;
    }

    const std::string windowId = "PlayerSettings";
    setNextChildWindowSize(windowId, ImVec2(500.0f, 420.0f));
    if (ImGui::Begin("Player Settings", &showPlayerSettingsWindow_)) {
        updateChildWindowSizeState(windowId);
        renderPlayerSettings();
    }
    ImGui::End();
}

void MainWindow::renderPluginSelectorWindow() {
    if (!showPluginSelectorWindow_) {
        return;
    }

    const std::string windowId = "PluginSelector";
    setNextChildWindowSize(windowId, ImVec2(520.0f, 560.0f));
    if (ImGui::Begin("Plugin Selector", &showPluginSelectorWindow_)) {
        updateChildWindowSizeState(windowId);
        renderPluginSelector();
    }
    ImGui::End();
}

void MainWindow::update() {
}

void MainWindow::applyUiScale(float scale) {
    uiScale_ = std::clamp(scale, 0.5f, 4.0f);

    ImGuiStyle& style = ImGui::GetStyle();
    style = baseStyle_;
    style.ScaleAllSizes(uiScale_);

    applyFontScaling();
    uiScaleDirty_ = true;
}

void MainWindow::requestWindowResize() {
    ImGuiIO& io = ImGui::GetIO();
    if (baseWindowSize_.x <= 0.0f || baseWindowSize_.y <= 0.0f) {
        if (io.DisplaySize.x > 0.0f && io.DisplaySize.y > 0.0f) {
            baseWindowSize_.x = io.DisplaySize.x / std::max(uiScale_, 0.0001f);
            baseWindowSize_.y = io.DisplaySize.y / std::max(uiScale_, 0.0001f);
        }
    }

    requestedWindowSize_.x = std::max(200.0f, baseWindowSize_.x * uiScale_);
    requestedWindowSize_.y = std::max(200.0f, baseWindowSize_.y * uiScale_);
    windowSizeRequestPending_ = true;
    waitingForWindowResize_ = true;
}

bool MainWindow::consumePendingWindowResize(ImVec2& size) {
    if (!windowSizeRequestPending_) {
        return false;
    }
    size = requestedWindowSize_;
    windowSizeRequestPending_ = false;
    return true;
}

void MainWindow::captureFontScales() {
    auto& io = ImGui::GetIO();
    baseFontScales_.clear();
    baseFontScales_.reserve(static_cast<size_t>(io.Fonts->Fonts.Size));
    for (int i = 0; i < io.Fonts->Fonts.Size; ++i) {
        baseFontScales_.push_back(io.Fonts->Fonts[i]->Scale);
    }
    fontScalesCaptured_ = true;
}

void MainWindow::applyFontScaling() {
    auto& io = ImGui::GetIO();
    if (!fontScalesCaptured_ || baseFontScales_.size() != static_cast<size_t>(io.Fonts->Fonts.Size)) {
        captureFontScales();
    }
    for (int i = 0; i < io.Fonts->Fonts.Size; ++i) {
        io.Fonts->Fonts[i]->Scale = baseFontScales_[i] * uiScale_;
    }
    io.FontGlobalScale = 1.0f;
}

void MainWindow::setNextChildWindowSize(const std::string& id, ImVec2 defaultBaseSize) {
    auto& state = childWindowSizes_[id];
    if (state.baseSize.x <= 0.0f || state.baseSize.y <= 0.0f) {
        state.baseSize = defaultBaseSize;
    }
    ImVec2 desired = ImVec2(state.baseSize.x * uiScale_, state.baseSize.y * uiScale_);
    ImGuiCond cond = uiScaleDirty_ ? ImGuiCond_Always : ImGuiCond_FirstUseEver;
    ImGui::SetNextWindowSize(desired, cond);
    if (uiScaleDirty_) {
        state.waitingForResize = true;
    }
}

void MainWindow::updateChildWindowSizeState(const std::string& id) {
    auto it = childWindowSizes_.find(id);
    if (it == childWindowSizes_.end()) {
        return;
    }

    auto& state = it->second;
    ImVec2 size = ImGui::GetWindowSize();
    constexpr float epsilon = 1.0f;
    if (state.waitingForResize) {
        ImVec2 target = ImVec2(state.baseSize.x * uiScale_, state.baseSize.y * uiScale_);
        if (std::fabs(size.x - target.x) < epsilon && std::fabs(size.y - target.y) < epsilon) {
            state.waitingForResize = false;
        }
    } else if (std::fabs(size.x - state.lastSize.x) > epsilon ||
               std::fabs(size.y - state.lastSize.y) > epsilon) {
        if (uiScale_ > 0.0f) {
            state.baseSize.x = size.x / uiScale_;
            state.baseSize.y = size.y / uiScale_;
        }
    }
    state.lastSize = size;
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

    // Check if this resize should be ignored (programmatic resize from plugin)
    // Don't erase yet - we only erase after processing a real user resize
    if (pluginWindowResizeIgnore_.find(instanceId) != pluginWindowResizeIgnore_.end()) {
        // Erase from ignore set so the next user resize will be processed
        pluginWindowResizeIgnore_.erase(instanceId);
        return;
    }

    auto* window = windowIt->second.get();
    if (!window)
        return;

    remidy::gui::Bounds currentBounds = pluginWindowBounds_[instanceId];

    auto& sequencer = uapmd::AppModel::instance().sequencer();
    pluginWindowBounds_[instanceId] = currentBounds;

    const uint32_t currentWidth = static_cast<uint32_t>(std::max(currentBounds.width, 0));
    const uint32_t currentHeight = static_cast<uint32_t>(std::max(currentBounds.height, 0));

    auto* instance = sequencer.getPluginInstance(instanceId);
    instance->setUISize(currentWidth, currentHeight);

    // Check if the plugin adjusted the size
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

void MainWindow::updateAudioDeviceSettingsData() {
    // Update sample rates for the selected devices
    auto manager = uapmd::AudioIODeviceManager::instance();

    // Get selected device indices
    int selectedInput = audioDeviceSettings_.getSelectedInputDevice();
    int selectedOutput = audioDeviceSettings_.getSelectedOutputDevice();

    // Get device names from the lists
    auto devices = manager->devices();

    std::string inputDeviceName;
    std::string outputDeviceName;

    int inputIndex = 0;
    for (auto& d : devices) {
        if (d.directions & UAPMD_AUDIO_DIRECTION_INPUT) {
            if (inputIndex == selectedInput) {
                inputDeviceName = d.name;
                break;
            }
            inputIndex++;
        }
    }

    int outputIndex = 0;
    for (auto& d : devices) {
        if (d.directions & UAPMD_AUDIO_DIRECTION_OUTPUT) {
            if (outputIndex == selectedOutput) {
                outputDeviceName = d.name;
                break;
            }
            outputIndex++;
        }
    }

    // Get sample rates for selected devices
    auto inputSampleRates = manager->getDeviceSampleRates(inputDeviceName, UAPMD_AUDIO_DIRECTION_INPUT);
    auto outputSampleRates = manager->getDeviceSampleRates(outputDeviceName, UAPMD_AUDIO_DIRECTION_OUTPUT);

    audioDeviceSettings_.setInputAvailableSampleRates(inputSampleRates);
    audioDeviceSettings_.setOutputAvailableSampleRates(outputSampleRates);
}

void MainWindow::renderPlayerSettings() {
    auto& sequencer = uapmd::AppModel::instance().sequencer();

    ImGui::Text("Current File: %s", currentFile_.empty() ? "None" : currentFile_.c_str());

    if (ImGui::Button("Load File...")) {
        loadFile();
    }

    ImGui::SameLine();

    // Disable Unload button if no file is loaded
    bool hasFile = !currentFile_.empty();
    if (!hasFile) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Unload File")) {
        unloadFile();
    }
    if (!hasFile) {
        ImGui::EndDisabled();
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

    // Update the track list data and render
    updateTrackListData();
    trackList_.render();
}

std::optional<TrackInstance> MainWindow::buildTrackInstanceInfo(int32_t instanceId) {
    auto& sequencer = uapmd::AppModel::instance().sequencer();
    auto* instance = sequencer.getPluginInstance(instanceId);
    if (!instance) {
        return std::nullopt;
    }

    int32_t trackIndex = sequencer.findTrackIndexForInstance(instanceId);
    std::string pluginName = sequencer.getPluginName(instanceId);
    std::string pluginFormat = sequencer.getPluginFormat(instanceId);

    if (umpDeviceNameBuffers_.find(instanceId) == umpDeviceNameBuffers_.end()) {
        // Initialize the buffer from device state label if available, otherwise use default
        std::string initialName;

        bool labelFound = false;
        auto* deviceController = uapmd::AppModel::instance().deviceController();
        if (deviceController) {
            std::lock_guard lock(devicesMutex_);
            for (auto& entry : devices_) {
                auto state = entry.state;
                std::lock_guard guard(state->mutex);
                if (state->pluginInstances.count(instanceId) > 0) {
                    if (!state->label.empty()) {
                        initialName = state->label;
                        labelFound = true;
                    }
                    break;
                }
            }
        }

        if (!labelFound) {
            initialName = std::format("{} [{}]", pluginName, pluginFormat);
        }

        umpDeviceNameBuffers_[instanceId] = {};
        std::strncpy(umpDeviceNameBuffers_[instanceId].data(), initialName.c_str(),
                     umpDeviceNameBuffers_[instanceId].size() - 1);
        umpDeviceNameBuffers_[instanceId][umpDeviceNameBuffers_[instanceId].size() - 1] = '\0';
    }

    bool deviceRunning = false;
    bool deviceExists = false;
    bool deviceInstantiating = false;

    auto* deviceController = uapmd::AppModel::instance().deviceController();
    if (deviceController) {
        std::lock_guard lock(devicesMutex_);
        for (auto& entry : devices_) {
            auto state = entry.state;
            std::lock_guard guard(state->mutex);
            if (state->pluginInstances.count(instanceId) > 0) {
                deviceExists = true;
                deviceRunning = state->running;
                deviceInstantiating = state->instantiating;
                break;
            }
        }
    }

    TrackInstance ti;
    ti.instanceId = instanceId;
    ti.trackIndex = trackIndex;
    ti.pluginName = pluginName;
    ti.pluginFormat = pluginFormat;
    ti.umpDeviceName = std::string(umpDeviceNameBuffers_[instanceId].data());
    ti.hasUI = instance->hasUISupport();
    ti.uiVisible = instance->isUIVisible();
    auto detailsIt = detailsWindows_.find(instanceId);
    ti.detailsVisible = detailsIt != detailsWindows_.end() && detailsIt->second.visible;
    ti.deviceRunning = deviceRunning;
    ti.deviceExists = deviceExists;
    ti.deviceInstantiating = deviceInstantiating;

    return ti;
}

void MainWindow::updateTrackListData() {
    auto& sequencer = uapmd::AppModel::instance().sequencer();

    std::vector<TrackInstance> trackInstances;

    for (int32_t instanceId : instances_) {
        if (auto trackInstance = buildTrackInstanceInfo(instanceId)) {
            trackInstances.push_back(*trackInstance);
        }
    }

    trackList_.setInstances(trackInstances);
}

void MainWindow::refreshDeviceList() {
    std::vector<std::string> inputDevices;
    std::vector<std::string> outputDevices;

    auto manager = uapmd::AudioIODeviceManager::instance();
    auto devices = manager->devices();

    for (auto& d : devices) {
        if (d.directions & UAPMD_AUDIO_DIRECTION_INPUT) {
            inputDevices.push_back(d.name);
        }
        if (d.directions & UAPMD_AUDIO_DIRECTION_OUTPUT) {
            outputDevices.push_back(d.name);
        }
    }

    audioDeviceSettings_.setInputDevices(inputDevices);
    audioDeviceSettings_.setOutputDevices(outputDevices);

    // Reset selection if out of bounds FIRST
    int selectedInput = audioDeviceSettings_.getSelectedInputDevice();
    int selectedOutput = audioDeviceSettings_.getSelectedOutputDevice();

    if (selectedInput >= static_cast<int>(inputDevices.size())) {
        audioDeviceSettings_.setSelectedInputDevice(0);
    }
    if (selectedOutput >= static_cast<int>(outputDevices.size())) {
        audioDeviceSettings_.setSelectedOutputDevice(0);
    }

    // Get sample rates from the opened audio device and update the UI
    updateAudioDeviceSettingsData();
}

void MainWindow::handleAudioDeviceChange() {
    // Get selected device indices
    int selectedInput = audioDeviceSettings_.getSelectedInputDevice();
    int selectedOutput = audioDeviceSettings_.getSelectedOutputDevice();

    // Get selected sample rate (use output sample rate as the primary)
    uint32_t sampleRate = static_cast<uint32_t>(audioDeviceSettings_.getOutputSampleRate());

    // Get selected buffer size
    uint32_t bufferSize = static_cast<uint32_t>(audioDeviceSettings_.getBufferSize());

    // Update the UI with sample rates for the newly selected device
    updateAudioDeviceSettingsData();

    // Reconfigure the audio device in the sequencer with the selected device indices, sample rate, and buffer size
    auto& sequencer = uapmd::AppModel::instance().sequencer();
    if (!sequencer.reconfigureAudioDevice(selectedInput, selectedOutput, sampleRate, bufferSize)) {
        std::cerr << "Failed to reconfigure audio device" << std::endl;
    } else {
        std::cout << std::format("Audio device reconfigured: Input Index={}, Output Index={}, Sample Rate={}, Buffer Size={}",
                                selectedInput,
                                selectedOutput,
                                sampleRate,
                                bufferSize) << std::endl;
    }
}

void MainWindow::applyDeviceSettings() {
    // TODO: Apply settings to the actual audio system
    // This would typically involve:
    // - Stopping current audio
    // - Reconfiguring the audio system with new settings
    // - Restarting audio

    std::cout << std::format("Applied audio settings: Input Index={}, Output Index={}, Input SR={}, Output SR={}, BS={}",
                            audioDeviceSettings_.getSelectedInputDevice(),
                            audioDeviceSettings_.getSelectedOutputDevice(),
                            audioDeviceSettings_.getInputSampleRate(),
                            audioDeviceSettings_.getOutputSampleRate(),
                            audioDeviceSettings_.getBufferSize()) << std::endl;
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

    auto reader = uapmd::createAudioFileReaderFromPath(filepath);
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

    std::cout << "File loaded: " << currentFile_ << std::endl;
}

void MainWindow::unloadFile() {
    auto& sequencer = uapmd::AppModel::instance().sequencer();

    // Stop playback if currently playing
    if (isPlaying_) {
        stop();
    }

    // Unload the audio file from the sequencer
    sequencer.unloadAudioFile();

    // Clear UI state
    currentFile_.clear();
    playbackLength_ = 0.0f;
    playbackPosition_ = 0.0f;

    std::cout << "Audio file unloaded" << std::endl;
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
    auto* pal = uapmd::AppModel::instance().sequencer().getPluginInstance(instanceId);
    if (!pal) {
        return;
    }

    auto* parameterSupport = pal->parameterSupport();
    auto perNoteSelection = buildPerNoteSelection(state.parameterList);
    const bool usingPerNoteControllers = perNoteSelection.has_value();

    if (usingPerNoteControllers && !parameterSupport) {
        state.parameterList.setParameters({});
        return;
    }

    std::vector<uapmd::ParameterMetadata> parameters;
    if (usingPerNoteControllers) {
        auto pluginParams = parameterSupport->perNoteControllers(perNoteSelection->type, perNoteSelection->context);
        parameters = toParameterMetadata(pluginParams);
    } else {
        parameters = pal->parameterMetadataList();
    }

    state.parameterList.setParameters(parameters);
    if (parameters.empty()) {
        if (!usingPerNoteControllers) {
            applyParameterUpdates(instanceId, state);
        }
        return;
    }

    for (size_t i = 0; i < parameters.size(); ++i) {
        double initialValue = parameters[i].defaultPlainValue;
        if (parameterSupport) {
            double queriedValue = initialValue;
            auto status = usingPerNoteControllers
                              ? parameterSupport->getPerNoteController(perNoteSelection->context, parameters[i].index, &queriedValue)
                              : parameterSupport->getParameter(parameters[i].index, &queriedValue);
            if (status == remidy::StatusCode::OK) {
                initialValue = queriedValue;
            }
        }
        state.parameterList.setParameterValue(i, static_cast<float>(initialValue));

        // Update value string
        auto valueString = (usingPerNoteControllers && perNoteSelection)
                               ? pal->getPerNoteControllerValueString(
                                     static_cast<uint8_t>(perNoteSelection->context.note),
                                     static_cast<uint8_t>(parameters[i].index),
                                     initialValue)
                               : pal->getParameterValueString(parameters[i].index, initialValue);
        state.parameterList.setParameterValueString(i, valueString);
    }

    if (!usingPerNoteControllers) {
        applyParameterUpdates(instanceId, state);
    }
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

void MainWindow::applyParameterUpdates(int32_t instanceId, DetailsWindowState& state) {
    if (state.parameterList.context() != ParameterList::ParameterContext::Global) {
        return;
    }
    auto& sequencer = uapmd::AppModel::instance().sequencer();
    auto* pal = sequencer.getPluginInstance(instanceId);
    if (!pal) {
        return;
    }

    auto updates = sequencer.getParameterUpdates(instanceId);
    const auto& parameters = state.parameterList.getParameters();

    for (const auto& update : updates) {
        for (size_t i = 0; i < parameters.size(); ++i) {
            if (parameters[i].index == update.parameterIndex) {
                state.parameterList.setParameterValue(i, static_cast<float>(update.value));
                auto valueString = pal->getParameterValueString(parameters[i].index, update.value);
                state.parameterList.setParameterValueString(i, valueString);
                break;
            }
        }
    }
}

void MainWindow::renderParameterControls(int32_t instanceId, DetailsWindowState& state) {
    applyParameterUpdates(instanceId, state);

    // Render the parameter list component
    state.parameterList.render();
}

void MainWindow::refreshPluginList() {
    std::vector<PluginEntry> plugins;

    auto& catalog = uapmd::AppModel::instance().sequencer().catalog();
    auto catalogPlugins = catalog.getPlugins();

    for (auto* plugin : catalogPlugins) {
        plugins.push_back({
            .format = plugin->format(),
            .id = plugin->pluginId(),
            .name = plugin->displayName(),
            .vendor = plugin->vendorName()
        });
    }

    pluginList_.setPlugins(plugins);
}

void MainWindow::renderPluginSelector() {
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

    // Render the plugin list component
    pluginList_.render();

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

        // Always create device (creates UMP device even for existing tracks)
        createDeviceForPlugin(selection.format, selection.pluginId, trackIndex);
        showPluginSelectorWindow_ = false;
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
            // Route directly to this instance to avoid ambiguity on tracks with multiple plugins
            if (isPressed) {
                seq.sendNoteOn(instanceId, note);
            } else {
                seq.sendNoteOff(instanceId, note);
            }
        });

        // Set up parameter list callbacks
        state.parameterList.setOnParameterChanged([this, instanceId](uint32_t parameterIndex, float value) {
            auto& seq = uapmd::AppModel::instance().sequencer();
            auto perNoteSelection = [this, instanceId]() -> std::optional<PerNoteSelection> {
                auto it = detailsWindows_.find(instanceId);
                if (it == detailsWindows_.end()) {
                    return std::nullopt;
                }
                return buildPerNoteSelection(it->second.parameterList);
            }();

            if (!perNoteSelection) {
                seq.setParameterValue(instanceId, parameterIndex, value);
                return;
            }

            auto* pal = seq.getPluginInstance(instanceId);
            if (!pal) {
                return;
            }
            if (perNoteSelection->type == remidy::PerNoteControllerContextTypes::PER_NOTE_CONTROLLER_PER_NOTE) {
                pal->setPerNoteControllerValue(
                    static_cast<uint8_t>(perNoteSelection->context.note),
                    static_cast<uint8_t>(parameterIndex),
                    value);
                return;
            }
            if (auto* parameterSupport = pal->parameterSupport())
                parameterSupport->setPerNoteController(perNoteSelection->context, parameterIndex, value, 0);
        });

        state.parameterList.setOnGetParameterValueString([this, instanceId](uint32_t parameterIndex, float value) -> std::string {
            auto& seq = uapmd::AppModel::instance().sequencer();
            auto* pal = seq.getPluginInstance(instanceId);
            if (pal) {
                auto perNoteSelection = [this, instanceId]() -> std::optional<PerNoteSelection> {
                    auto it = detailsWindows_.find(instanceId);
                    if (it == detailsWindows_.end())
                        return std::nullopt;
                    return buildPerNoteSelection(it->second.parameterList);
                }();

                if (perNoteSelection) {
                    return pal->getPerNoteControllerValueString(
                        static_cast<uint8_t>(perNoteSelection->context.note),
                        static_cast<uint8_t>(parameterIndex),
                        value);
                }
                return pal->getParameterValueString(parameterIndex, value);
            }
            return "";
        });

        state.parameterList.setOnContextChanged([this, instanceId](ParameterList::ParameterContext, uint8_t) {
            auto it = detailsWindows_.find(instanceId);
            if (it == detailsWindows_.end()) {
                return;
            }
            refreshParameters(instanceId, it->second);
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

    std::vector<int32_t> detailIds;
    detailIds.reserve(detailsWindows_.size());
    for (const auto& entry : detailsWindows_) {
        detailIds.push_back(entry.first);
    }

    for (int32_t instanceId : detailIds) {
        auto it = detailsWindows_.find(instanceId);
        if (it == detailsWindows_.end()) {
            continue;
        }

        auto& detailsState = it->second;
        if (!detailsState.visible) {
            continue;
        }

        // Create ImGui window for this instance's details
        std::string windowTitle = sequencer.getPluginName(instanceId) + " (" +
                                 sequencer.getPluginFormat(instanceId) + ") - Details###Details" +
                                 std::to_string(instanceId);

        bool windowOpen = detailsState.visible;
        bool deleteRequested = false;
        std::string windowSizeId = std::format("DetailsWindow{}", instanceId);
        float baseWidth = 600.0f;
        const float viewportWidth = ImGui::GetIO().DisplaySize.x;
        if (viewportWidth > 0.0f && uiScale_ > 0.0f) {
            baseWidth = std::min(baseWidth, viewportWidth / uiScale_);
        }
        setNextChildWindowSize(windowSizeId, ImVec2(baseWidth, 500.0f));
        if (ImGui::Begin(windowTitle.c_str(), &windowOpen)) {
            updateChildWindowSizeState(windowSizeId);
            auto* instance = sequencer.getPluginInstance(instanceId);
            if (!instance) {
                ImGui::TextUnformatted("Instance is no longer available.");
            } else {
                if (auto trackInstance = buildTrackInstanceInfo(instanceId)) {
                    bool disableShowUIButton = !trackInstance->hasUI;
                    if (disableShowUIButton) {
                        ImGui::BeginDisabled();
                    }
                    const char* uiButtonText = trackInstance->uiVisible ? "Hide UI" : "Show UI";
                    if (ImGui::Button(uiButtonText)) {
                        if (trackInstance->uiVisible) {
                            handleHideUI(instanceId);
                        } else {
                            handleShowUI(instanceId);
                        }
                    }
                    if (disableShowUIButton) {
                        ImGui::EndDisabled();
                    }

                    ImGui::SameLine();

                    ImGui::SameLine();
                }

                if (ImGui::Button("Save State")) {
                    savePluginState(instanceId);
                }

                ImGui::SameLine();
                if (ImGui::Button("Load State")) {
                    loadPluginState(instanceId);
                }

                ImGui::SameLine();
                if (ImGui::Button("Delete")) {
                    deleteRequested = true;
                }

                ImGui::Separator();

                if (detailsState.parameterList.getParameters().empty()) {
                    refreshParameters(instanceId, detailsState);
                }
                if (detailsState.presets.empty()) {
                    refreshPresets(instanceId, detailsState);
                }

                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted("Pitchbend:");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(140.0f);
                if (ImGui::SliderFloat("##Pitchbend", &detailsState.pitchBendValue,
                                       -1.0f, 1.0f, "%.2f", ImGuiSliderFlags_NoInput)) {
                    sendPitchBend(instanceId, detailsState.pitchBendValue);
                }
                ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted("Chan.Pressure:");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(150.0f);
                if (ImGui::SliderFloat("##ChanPressure", &detailsState.channelPressureValue,
                                       0.0f, 1.0f, "%.2f", ImGuiSliderFlags_NoInput)) {
                    sendChannelPressure(instanceId, detailsState.channelPressureValue);
                }
                detailsState.midiKeyboard.render();
                ImGui::Separator();

                ImGui::Text("Presets:");
                if (detailsState.presets.empty()) {
                    ImGui::TextDisabled("No presets available for this plugin.");
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
                                loadSelectedPreset(instanceId, detailsState);
                            }
                            if (isSelected) {
                                ImGui::SetItemDefaultFocus();
                            }
                        }
                        ImGui::EndCombo();
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

        if (deleteRequested) {
            handleRemoveInstance(instanceId);
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
    // Prepare configuration
    uapmd::AppModel::PluginInstanceConfig config;
    config.apiName = std::string(apiInput_);
    if (config.apiName.empty()) {
        config.apiName = "default";
    }
    config.deviceName = std::string(deviceNameInput_);  // Empty = auto-generate

    // Use AppModel's unified creation method
    // The global callback registered in constructor will handle UI updates
    uapmd::AppModel::instance().createPluginInstanceAsync(format, pluginId, trackIndex, config);
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

    // Build rows structure
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

    // Render row lambda
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

    // Render tracks
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

    // Render pending rows
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

    // Handle removal
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


void MainWindow::handleShowUI(int32_t instanceId) {
    auto& sequencer = uapmd::AppModel::instance().sequencer();
    auto* instance = sequencer.getPluginInstance(instanceId);
    if (!instance) return;

    std::string pluginName = sequencer.getPluginName(instanceId);
    std::string pluginFormat = sequencer.getPluginFormat(instanceId);

    // Create container window if needed
    auto windowIt = pluginWindows_.find(instanceId);
    remidy::gui::ContainerWindow* container = nullptr;
    if (windowIt == pluginWindows_.end()) {
        std::string windowTitle = pluginName + " (" + pluginFormat + ")";
        auto w = remidy::gui::ContainerWindow::create(windowTitle.c_str(), 800, 600, [this, instanceId]() {
            onPluginWindowClosed(instanceId);
        });
        container = w.get();
        w->setResizeCallback([this, instanceId](int width, int height) {
            pluginWindowBounds_[instanceId].width = width;
            pluginWindowBounds_[instanceId].height = height;
            onPluginWindowResized(instanceId);
        });
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
    bool pluginUIExists = (pluginWindowEmbedded_.find(instanceId) != pluginWindowEmbedded_.end());

    if (!pluginUIExists) {
        if (!instance->createUI(false, parentHandle,
            [this, instanceId](uint32_t w, uint32_t h){ return handlePluginResizeRequest(instanceId, w, h); })) {
            container->show(false);
            pluginWindows_.erase(instanceId);
            std::cout << "Failed to create plugin UI for instance " << instanceId << std::endl;
        } else {
            pluginWindowEmbedded_[instanceId] = true;
            bool canResize = instance->canUIResize();
            container->setResizable(canResize);
        }
    }

    if (pluginUIExists || pluginWindowEmbedded_[instanceId]) {
        if (!instance->showUI()) {
            std::cout << "Failed to show plugin UI for instance " << instanceId << std::endl;
        }
    }
}

void MainWindow::handleHideUI(int32_t instanceId) {
    auto& sequencer = uapmd::AppModel::instance().sequencer();
    auto* instance = sequencer.getPluginInstance(instanceId);
    if (!instance) return;

    instance->hideUI();
    auto windowIt = pluginWindows_.find(instanceId);
    if (windowIt != pluginWindows_.end()) {
        windowIt->second->show(false);
    }
}

void MainWindow::handleEnableDevice(int32_t instanceId, const std::string& deviceName) {
    // Update the device label in the UI state
    {
        std::lock_guard lock(devicesMutex_);
        for (auto& entry : devices_) {
            auto state = entry.state;
            std::lock_guard guard(state->mutex);
            if (state->pluginInstances.count(instanceId) > 0) {
                state->label = deviceName;
                break;
            }
        }
    }

    // Enable the device - this will trigger the global callback to update running state
    uapmd::AppModel::instance().enableUmpDevice(instanceId, deviceName);
}

void MainWindow::handleDisableDevice(int32_t instanceId) {
    // Disable the device - this will trigger the global callback to update running state
    uapmd::AppModel::instance().disableUmpDevice(instanceId);
}

void MainWindow::handleRemoveInstance(int32_t instanceId) {
    auto& sequencer = uapmd::AppModel::instance().sequencer();
    auto* instance = sequencer.getPluginInstance(instanceId);
    if (!instance) return;

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

    // Remove the plugin instance from AppModel
    // This will trigger the global callback that updates devices_ and calls refreshInstances()
    uapmd::AppModel::instance().removePluginInstance(instanceId);

    std::cout << "Removed plugin instance: " << instanceId << std::endl;
}

void MainWindow::sendPitchBend(int32_t instanceId, float normalizedValue) {
    auto& seq = uapmd::AppModel::instance().sequencer();
    auto trackIdx = seq.findTrackIndexForInstance(instanceId);
    if (trackIdx < 0) {
        return;
    }

    float clamped = std::clamp((normalizedValue + 1.0f) * 0.5f, 0.0f, 1.0f);
    uint32_t pitchValue = static_cast<uint32_t>(clamped * 4294967295.0f);
    uapmd_ump_t buffer[2];
    uint64_t ump = cmidi2_ump_midi2_pitch_bend_direct(0, 0, pitchValue);
    buffer[0] = static_cast<uapmd_ump_t>(ump >> 32);
    buffer[1] = static_cast<uapmd_ump_t>(ump & 0xFFFFFFFFu);
    seq.enqueueUmp(trackIdx, buffer, sizeof(buffer), 0);
}

void MainWindow::sendChannelPressure(int32_t instanceId, float pressure) {
    auto& seq = uapmd::AppModel::instance().sequencer();
    auto trackIdx = seq.findTrackIndexForInstance(instanceId);
    if (trackIdx < 0) {
        return;
    }

    float clamped = std::clamp(pressure, 0.0f, 1.0f);
    uint32_t pressureValue = static_cast<uint32_t>(clamped * 4294967295.0f);
    uapmd_ump_t buffer[2];
    uint64_t ump = cmidi2_ump_midi2_caf(0, 0, pressureValue);
    buffer[0] = static_cast<uapmd_ump_t>(ump >> 32);
    buffer[1] = static_cast<uapmd_ump_t>(ump & 0xFFFFFFFFu);
    seq.enqueueUmp(trackIdx, buffer, sizeof(buffer), 0);
}

}
