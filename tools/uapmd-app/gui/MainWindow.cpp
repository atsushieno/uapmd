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
#include <limits>
#include <portable-file-dialogs.h>

#include <midicci/midicci.hpp> // include before anything that indirectly includes X.h

#include <imgui.h>

#include "SharedTheme.hpp"
#include "FontLoader.hpp"

#include "MainWindow.hpp"
#include "../AppModel.hpp"

namespace uapmd::gui {
MainWindow::MainWindow(GuiDefaults defaults) {
    SetupImGuiStyle();
    ensureApplicationFont();
    baseStyle_ = ImGui::GetStyle();
    captureFontScales();
    applyUiScale(uiScale_);

    // Apply defaults from command line arguments
    // TODO: If needed, implement default plugin selection through pluginList_

    // Set up spectrum analyzer data providers
    inputSpectrumAnalyzer_.setDataProvider([this](float* data, int dataSize) {
        uapmd::AppModel::instance().sequencer().engine()->getInputSpectrum(data, dataSize);
    });
    outputSpectrumAnalyzer_.setDataProvider([this](float* data, int dataSize) {
        uapmd::AppModel::instance().sequencer().engine()->getOutputSpectrum(data, dataSize);
    });

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

            // AppModel now creates DeviceState, we just initialize GUI-specific state
            // Initialize UMP device name buffer
            auto& sequencer = uapmd::AppModel::instance().sequencer();
            std::string pluginFormat = sequencer.getPluginFormat(result.instanceId);
            std::string deviceLabel = result.device ? std::format("{} [{}]", result.pluginName, pluginFormat) : "";
            umpDeviceNameBuffers_[result.instanceId] = {};
            std::strncpy(umpDeviceNameBuffers_[result.instanceId].data(), deviceLabel.c_str(),
                         umpDeviceNameBuffers_[result.instanceId].size() - 1);
            umpDeviceNameBuffers_[result.instanceId][umpDeviceNameBuffers_[result.instanceId].size() - 1] = '\0';

            // Refresh UI to display new instance
            refreshInstances();
            trackList_.markDirty();
        });

    // Register callback for when plugin instances are removed (GUI or script)
    uapmd::AppModel::instance().instanceRemoved.push_back(
        [this](int32_t instanceId) {
            // Cleanup plugin UI windows
            auto windowIt = pluginWindows_.find(instanceId);
            if (windowIt != pluginWindows_.end()) {
                windowIt->second->show(false);
                pluginWindows_.erase(windowIt);
            }
            pluginWindowEmbedded_.erase(instanceId);
            pluginWindowBounds_.erase(instanceId);
            pluginWindowResizeIgnore_.erase(instanceId);

            // Cleanup details window if open
            instanceDetails_.removeInstance(instanceId);

            // Cleanup stale Sequence Editor windows
            // Get the current number of tracks and remove windows for deleted track indices
            auto& sequencer = uapmd::AppModel::instance().sequencer();
            auto tracks = sequencer.engine()->getTrackInfos();
            int32_t maxValidTrackIndex = tracks.empty() ? -1 : static_cast<int32_t>(tracks.size() - 1);
            sequenceEditor_.removeStaleWindows(maxValidTrackIndex);

            // AppModel handles removing from devices_ - we just refresh UI
            refreshInstances();
            trackList_.markDirty();
        });

    // Register callback for when devices are enabled
    uapmd::AppModel::instance().enableDeviceCompleted.push_back(
        [this](const uapmd::AppModel::DeviceStateResult& result) {
            // AppModel updates DeviceState directly - we just refresh UI
            refreshInstances();
            trackList_.markDirty();
        });

    // Register callback for when devices are disabled
    uapmd::AppModel::instance().disableDeviceCompleted.push_back(
        [this](const uapmd::AppModel::DeviceStateResult& result) {
            // AppModel updates DeviceState directly - we just refresh UI
            refreshInstances();
            trackList_.markDirty();
        });

    // Register callback for when scripts request to show UI
    uapmd::AppModel::instance().uiShowRequested.push_back(
        [this](int32_t instanceId) {
            // Handle the request by calling our handleShowUI method
            handleShowUI(instanceId);
        });

    // Register callback for when plugin UIs are shown (from scripts or GUI)
    uapmd::AppModel::instance().uiShown.push_back(
        [this](const uapmd::AppModel::UIStateResult& result) {
            if (!result.success) {
                // Show error, hide container window if it was created
                std::cout << "Failed to show plugin UI: " << result.error << std::endl;
                auto windowIt = pluginWindows_.find(result.instanceId);
                if (windowIt != pluginWindows_.end()) {
                    windowIt->second->show(false);
                }
                return;
            }

            auto& sequencer = uapmd::AppModel::instance().sequencer();
            auto* instance = sequencer.engine()->getPluginInstance(result.instanceId);
            if (!instance) return;

            // Mark as embedded and configure container window if UI was just created
            if (result.wasCreated) {
                pluginWindowEmbedded_[result.instanceId] = true;

                auto windowIt = pluginWindows_.find(result.instanceId);
                if (windowIt != pluginWindows_.end()) {
                    bool canResize = instance->canUIResize();
                    windowIt->second->setResizable(canResize);
                }
            }
            trackList_.markDirty();
        });

    // Register callback for when plugin UIs are hidden (from scripts or GUI)
    uapmd::AppModel::instance().uiHidden.push_back(
        [this](const uapmd::AppModel::UIStateResult& result) {
            if (!result.success) return;

            // Call handleHideUI to process the window state changes
            handleHideUI(result.instanceId);
            trackList_.markDirty();
        });

    // Set up TrackList callbacks
    trackList_.setOnBuildTrackInstance([this](int32_t instanceId) -> std::optional<TrackInstance> {
        return buildTrackInstanceInfo(instanceId);
    });

    trackList_.setOnShowDetails([this](int32_t instanceId) {
        instanceDetails_.showWindow(instanceId);
        trackList_.markDirty();
    });

    trackList_.setOnHideDetails([this](int32_t instanceId) {
        instanceDetails_.hideWindow(instanceId);
        trackList_.markDirty();
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

    trackList_.setOnShowSequence([this](int32_t trackIndex) {
        sequenceEditor_.showWindow(trackIndex);
        // Refresh clips for this track
        refreshSequenceEditorForTrack(trackIndex);
        trackList_.markDirty();
    });

    trackList_.setOnHideSequence([this](int32_t trackIndex) {
        sequenceEditor_.hideWindow(trackIndex);
        trackList_.markDirty();
    });

    // Set up PluginSelector callbacks
    pluginSelector_.setOnInstantiatePlugin([this](const std::string& format, const std::string& pluginId, int32_t trackIndex) {
        createDeviceForPlugin(format, pluginId, trackIndex);
        showPluginSelectorWindow_ = false;
    });

    pluginSelector_.setOnScanPlugins([](bool forceRescan) {
        uapmd::AppModel::instance().performPluginScanning(forceRescan);
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
        if (ImGui::BeginChild("MainToolbar", ImVec2(0, 90.0f * uiScale_), false, ImGuiWindowFlags_NoScrollbar)) {
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
    InstanceDetails::RenderContext detailsContext{
        .buildTrackInstance = [this](int32_t instanceId) -> std::optional<TrackInstance> {
            return buildTrackInstanceInfo(instanceId);
        },
        .savePluginState = [this](int32_t instanceId) {
            savePluginState(instanceId);
        },
        .loadPluginState = [this](int32_t instanceId) {
            loadPluginState(instanceId);
        },
        .removeInstance = [this](int32_t instanceId) {
            handleRemoveInstance(instanceId);
        },
        .setNextChildWindowSize = [this](const std::string& id, ImVec2 defaultSize) {
            setNextChildWindowSize(id, defaultSize);
        },
        .updateChildWindowSizeState = [this](const std::string& id) {
            updateChildWindowSizeState(id);
        },
        .onWindowClosed = [this](int32_t instanceId) {
            trackList_.markDirty();
        },
        .uiScale = uiScale_,
    };
    instanceDetails_.render(detailsContext);

    // Render SequenceEditor
    SequenceEditor::RenderContext seqContext{
        .refreshClips = [this](int32_t trackIndex) {
            refreshSequenceEditorForTrack(trackIndex);
        },
        .addClip = [this](int32_t trackIndex, const std::string& filepath) {
            addClipToTrack(trackIndex, filepath);
        },
        .removeClip = [this](int32_t trackIndex, int32_t clipId) {
            removeClipFromTrack(trackIndex, clipId);
        },
        .clearAllClips = [this](int32_t trackIndex) {
            clearAllClipsFromTrack(trackIndex);
        },
        .updateClip = [this](int32_t trackIndex, int32_t clipId, int32_t anchorId, const std::string& origin, const std::string& position) {
            updateClip(trackIndex, clipId, anchorId, origin, position);
        },
        .updateClipName = [this](int32_t trackIndex, int32_t clipId, const std::string& name) {
            updateClipName(trackIndex, clipId, name);
        },
        .changeClipFile = [this](int32_t trackIndex, int32_t clipId) {
            changeClipFile(trackIndex, clipId);
        },
        .moveClipAbsolute = [this](int32_t trackIndex, int32_t clipId, double seconds) {
            moveClipAbsolute(trackIndex, clipId, seconds);
        },
        .setNextChildWindowSize = [this](const std::string& id, ImVec2 defaultSize) {
            setNextChildWindowSize(id, defaultSize);
        },
        .updateChildWindowSizeState = [this](const std::string& id) {
            updateChildWindowSizeState(id);
        },
        .uiScale = uiScale_,
    };
    sequenceEditor_.render(seqContext);

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

        // Update track options before rendering
        std::vector<TrackDestinationOption> trackOptions;
        auto& sequencer = uapmd::AppModel::instance().sequencer();
        auto tracks = sequencer.engine()->getTrackInfos();
        for (const auto& track : tracks) {
            TrackDestinationOption option{
                .trackIndex = track.trackIndex,
                .label = std::format("Track {}", track.trackIndex + 1)
            };
            trackOptions.push_back(std::move(option));
        }
        pluginSelector_.setTrackOptions(trackOptions);
        pluginSelector_.setScanning(uapmd::AppModel::instance().isScanning());

        pluginSelector_.render();
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

    auto* instance = sequencer.engine()->getPluginInstance(instanceId);
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

    auto* instance = sequencer.engine()->getPluginInstance(instanceId);
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
    sequencer.engine()->getPluginInstance(instanceId)->hideUI();

    // Update our visibility tracking so the button text is correct
    pluginWindowVisible_[instanceId] = false;
}

bool MainWindow::fetchPluginUISize(int32_t instanceId, uint32_t &width, uint32_t &height) {
    auto& sequencer = uapmd::AppModel::instance().sequencer();
    auto* instance = sequencer.engine()->getPluginInstance(instanceId);
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
    auto& model = uapmd::AppModel::instance();
    auto& transport = model.transport();
    auto& sequencer = model.sequencer();

    ImGui::Text("Current File: %s", transport.currentFile().empty() ? "None" : transport.currentFile().c_str());

    if (ImGui::Button("Load File...")) {
        auto selection = pfd::open_file(
            "Select Audio File",
            ".",
            { "Audio Files", "*.wav *.flac *.ogg",
              "WAV Files", "*.wav",
              "FLAC Files", "*.flac",
              "OGG Files", "*.ogg",
              "All Files", "*" }
        );

        if (!selection.result().empty()) {
            std::string filepath = selection.result()[0];
            std::string error = transport.loadFile(filepath);
            if (!error.empty())
                pfd::message("Load Failed", error, pfd::choice::ok, pfd::icon::error);
        }
    }

    ImGui::SameLine();

    // Disable Unload button if no file is loaded
    bool hasFile = !transport.currentFile().empty();
    if (!hasFile) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Unload File")) {
        transport.unloadFile();
    }
    if (!hasFile) {
        ImGui::EndDisabled();
    }

    // Spectrum analyzers - side by side
    float availableWidth = ImGui::GetContentRegionAvail().x;
    float spectrumWidth = (availableWidth - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    ImVec2 spectrumSize = ImVec2(spectrumWidth, 64);
    inputSpectrumAnalyzer_.setSize(spectrumSize);
    outputSpectrumAnalyzer_.setSize(spectrumSize);

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
        inputSpectrumAnalyzer_.render("##InputSpectrum");
        ImGui::TableNextColumn();
        outputSpectrumAnalyzer_.render("##OutputSpectrum");

        ImGui::EndTable();
    }

    ImGui::Text("Transport Controls:");

    // Play/Stop button
    const char* playStopButtonText = transport.isPlaying() ? "Stop" : "Play";
    if (ImGui::Button(playStopButtonText)) {
        if (transport.isPlaying())
            transport.stop();
        else
            transport.play();
    }

    ImGui::SameLine();

    // Pause/Resume button - only enabled when playing
    if (!transport.isPlaying()) {
        ImGui::BeginDisabled();
    }
    const char* pauseResumeButtonText = transport.isPaused() ? "Resume" : "Pause";
    if (ImGui::Button(pauseResumeButtonText)) {
        if (transport.isPaused())
            transport.resume();
        else
            transport.pause();
    }
    if (!transport.isPlaying()) {
        ImGui::EndDisabled();
    }

    ImGui::SameLine();
    const char* recordButtonText = transport.isRecording() ? "Stop Recording" : "Record";
    if (ImGui::Button(recordButtonText)) {
        transport.record();
    }

    ImGui::SameLine();
    bool offlineRendering = sequencer.engine()->offlineRendering();
    if (ImGui::Checkbox("Offline Rendering", &offlineRendering)) {
        sequencer.engine()->offlineRendering(offlineRendering);
    }

    // Position slider
    ImGui::Text("Position:");
    float position = transport.playbackPosition();
    float length = transport.playbackLength();
    if (ImGui::SliderFloat("##Position", &position, 0.0f, length, "%.1f s")) {
        std::cout << "Seeking to position: " << position << std::endl;
        // TODO: Add setPlaybackPosition to TransportController if seeking is needed
    }

    // Time display
    int currentMin = static_cast<int>(position) / 60;
    int currentSec = static_cast<int>(position) % 60;
    int totalMin = static_cast<int>(length) / 60;
    int totalSecTotal = static_cast<int>(length) % 60;

    ImGui::Text("Time: %02d:%02d / %02d:%02d", currentMin, currentSec, totalMin, totalSecTotal);

    ImGui::Text("Master Volume:");
    float volume = transport.volume();
    if (ImGui::SliderFloat("##Volume", &volume, 0.0f, 1.0f, "%.2f")) {
        transport.setVolume(volume);
        std::cout << "Volume changed to: " << volume << std::endl;
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
            sequencer.engine()->getPluginInstance(id)->destroyUI();
            pluginWindows_.erase(id);
            pluginWindowEmbedded_.erase(id);
            pluginWindowBounds_.erase(id);
            pluginWindowResizeIgnore_.erase(id);
        }
        pluginWindowsPendingClose_.clear();
    }

    // Update the track list data and render
    trackList_.update();
    trackList_.render();
}

std::optional<TrackInstance> MainWindow::buildTrackInstanceInfo(int32_t instanceId) {
    auto& sequencer = uapmd::AppModel::instance().sequencer();
    auto* instance = sequencer.engine()->getPluginInstance(instanceId);
    if (!instance) {
        return std::nullopt;
    }

    int32_t trackIndex = sequencer.findTrackIndexForInstance(instanceId);
    std::string pluginName = sequencer.engine()->getPluginName(instanceId);
    std::string pluginFormat = sequencer.getPluginFormat(instanceId);

    if (umpDeviceNameBuffers_.find(instanceId) == umpDeviceNameBuffers_.end()) {
        // Initialize the buffer from device state label if available, otherwise use default
        std::string initialName;
        bool labelFound = false;

        auto deviceState = uapmd::AppModel::instance().getDeviceForInstance(instanceId);
        if (deviceState && *deviceState) {
            std::lock_guard guard((*deviceState)->mutex);
            if (!(*deviceState)->label.empty()) {
                initialName = (*deviceState)->label;
                labelFound = true;
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
    bool deviceSupported = false;

    auto deviceState = uapmd::AppModel::instance().getDeviceForInstance(instanceId);
    if (deviceState && *deviceState) {
        std::lock_guard guard((*deviceState)->mutex);
        deviceExists = true;
        deviceRunning = (*deviceState)->running;
        deviceInstantiating = (*deviceState)->instantiating;
        deviceSupported = uapmd::midiApiSupportsUmp((*deviceState)->apiName);
    }

    TrackInstance ti;
    ti.instanceId = instanceId;
    ti.trackIndex = trackIndex;
    ti.pluginName = pluginName;
    ti.pluginFormat = pluginFormat;
    ti.umpDeviceName = std::string(umpDeviceNameBuffers_[instanceId].data());
    ti.hasUI = instance->hasUISupport();
    // Check if UI is actually visible by checking our visibility tracking
    // We track this ourselves because instance->isUIVisible() may not be reliable
    auto visIt = pluginWindowVisible_.find(instanceId);
    ti.uiVisible = (visIt != pluginWindowVisible_.end() && visIt->second);
    ti.detailsVisible = instanceDetails_.isVisible(instanceId);
    ti.sequenceVisible = sequenceEditor_.isVisible(trackIndex);
    ti.deviceRunning = deviceRunning;
    ti.deviceExists = deviceExists;
    ti.deviceInstantiating = deviceInstantiating;
    ti.deviceSupported = deviceSupported;

    return ti;
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

void MainWindow::refreshInstances() {
    // Get actual instance list from sequencer
    auto& sequencer = uapmd::AppModel::instance().sequencer();
    auto instances = sequencer.getInstanceIds();

    for (auto it = pluginWindows_.begin(); it != pluginWindows_.end();) {
        if (std::find(instances.begin(), instances.end(), it->first) == instances.end()) {
            it = pluginWindows_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = pluginWindowEmbedded_.begin(); it != pluginWindowEmbedded_.end();) {
        if (std::find(instances.begin(), instances.end(), it->first) == instances.end()) {
            it = pluginWindowEmbedded_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = pluginWindowBounds_.begin(); it != pluginWindowBounds_.end();) {
        if (std::find(instances.begin(), instances.end(), it->first) == instances.end()) {
            it = pluginWindowBounds_.erase(it);
        } else {
            ++it;
        }
    }
    instanceDetails_.pruneInvalidInstances(instances);
    std::vector<int32_t> resizeIgnoreRemove{};
    resizeIgnoreRemove.reserve(pluginWindowResizeIgnore_.size());
    for (auto id : pluginWindowResizeIgnore_) {
        if (std::find(instances.begin(), instances.end(), id) == instances.end())
            resizeIgnoreRemove.push_back(id);
    }
    for (auto id : resizeIgnoreRemove)
        pluginWindowResizeIgnore_.erase(id);

}

void MainWindow::refreshPluginList() {
    std::vector<PluginEntry> plugins;

    auto& catalog = uapmd::AppModel::instance().sequencer().engine()->pluginHost()->catalog();
    auto catalogPlugins = catalog.getPlugins();

    for (auto* plugin : catalogPlugins) {
        plugins.push_back({
            .format = plugin->format(),
            .id = plugin->pluginId(),
            .name = plugin->displayName(),
            .vendor = plugin->vendorName()
        });
    }

    pluginSelector_.setPlugins(plugins);
}

void MainWindow::savePluginState(int32_t instanceId) {
    auto& sequencer = uapmd::AppModel::instance().sequencer();

    std::string defaultFilename = std::format("{}.{}.state",
                                              sequencer.engine()->getPluginName(instanceId),
                                              sequencer.getPluginFormat(instanceId));
    std::ranges::replace(defaultFilename, ' ', '_');

    auto save = pfd::save_file(
        "Save Plugin State",
        defaultFilename,
        {"Plugin State Files", "*.state", "All Files", "*"}
    );

    std::string filepath = save.result();
    if (filepath.empty())
        return; // User cancelled

    // Delegate to AppModel for non-UI logic
    auto result = uapmd::AppModel::instance().savePluginState(instanceId, filepath);

    // Handle UI feedback based on result
    if (!result.success) {
        pfd::message("Save Failed",
            std::format("Failed to save plugin state:\n{}", result.error),
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
    if (filepaths.empty())
        return; // User cancelled

    std::string filepath = filepaths[0];

    // Delegate to AppModel for non-UI logic
    auto result = uapmd::AppModel::instance().loadPluginState(instanceId, filepath);

    // Handle UI feedback based on result
    if (!result.success) {
        pfd::message("Load Failed",
            std::format("Failed to load plugin state:\n{}", result.error),
            pfd::choice::ok,
            pfd::icon::error);
        return;
    }

    // Refresh parameters to reflect the loaded state if details window is open
    instanceDetails_.refreshParametersForInstance(instanceId);
}

void MainWindow::createDeviceForPlugin(const std::string& format, const std::string& pluginId, int32_t trackIndex) {
    // Prepare configuration
    uapmd::AppModel::PluginInstanceConfig config;
    config.apiName = std::string(pluginSelector_.getApiInput());
    if (config.apiName.empty()) {
        config.apiName = "default";
    }
    config.deviceName = std::string(pluginSelector_.getDeviceNameInput());  // Empty = auto-generate

    // Use AppModel's unified creation method
    // The global callback registered in constructor will handle UI updates
    uapmd::AppModel::instance().createPluginInstanceAsync(format, pluginId, trackIndex, config);
}


void MainWindow::handleShowUI(int32_t instanceId) {
    auto& sequencer = uapmd::AppModel::instance().sequencer();
    auto* instance = sequencer.engine()->getPluginInstance(instanceId);
    if (!instance) return;

    std::string pluginName = sequencer.engine()->getPluginName(instanceId);
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

    // Check if we need to create the UI (first time) or just show it (after hide)
    bool needsCreate = (pluginWindowEmbedded_.find(instanceId) == pluginWindowEmbedded_.end());

    // Call AppModel to create/show the UI - it will handle createUI() (if needed) and showUI()
    uapmd::AppModel::instance().showPluginUI(instanceId, needsCreate, false, parentHandle,
        [this, instanceId](uint32_t w, uint32_t h){ return handlePluginResizeRequest(instanceId, w, h); });

    // Mark as visible
    pluginWindowVisible_[instanceId] = true;
}

void MainWindow::handleHideUI(int32_t instanceId) {
    // AppModel::hidePluginUI() has already called instance->hideUI()
    // We just need to hide the window container - keep it so we can show again later
    auto windowIt = pluginWindows_.find(instanceId);
    if (windowIt != pluginWindows_.end()) {
        windowIt->second->show(false);
    }

    // Mark as not visible
    pluginWindowVisible_[instanceId] = false;

    // DO NOT erase pluginWindows_ or pluginWindowEmbedded_
    // The plugin UI and container still exist, just hidden
    // They will be destroyed when the plugin instance is actually removed
}

void MainWindow::handleEnableDevice(int32_t instanceId, const std::string& deviceName) {
    // Update device label in AppModel
    uapmd::AppModel::instance().updateDeviceLabel(instanceId, deviceName);

    // Enable device (AppModel will update state and trigger callback for UI refresh)
    uapmd::AppModel::instance().enableUmpDevice(instanceId, deviceName);
}

void MainWindow::handleDisableDevice(int32_t instanceId) {
    // Disable device (AppModel will update state and trigger callback for UI refresh)
    uapmd::AppModel::instance().disableUmpDevice(instanceId);
}

void MainWindow::handleRemoveInstance(int32_t instanceId) {
    // Remove the plugin instance from AppModel
    // This will:
    // 1. Hide and destroy plugin UI (in AppModel)
    // 2. Remove virtual MIDI device (in AppModel)
    // 3. Remove from sequencer (in AppModel)
    // 4. Trigger instanceRemoved callback which cleans up UI windows and devices list
    uapmd::AppModel::instance().removePluginInstance(instanceId);

    std::cout << "Removed plugin instance: " << instanceId << std::endl;
}

// Sequence Editor helpers
void MainWindow::refreshSequenceEditorForTrack(int32_t trackIndex) {
    auto& appModel = uapmd::AppModel::instance();
    auto tracks = appModel.getAppTracks();

    if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(tracks.size())) {
        return;
    }

    auto* track = tracks[trackIndex];
    auto clips = track->clipManager().getAllClips();

    // Sort clips by clipId to ensure chronological order (oldest first)
    std::sort(clips.begin(), clips.end(), [](const uapmd_app::ClipData* a, const uapmd_app::ClipData* b) {
        return a->clipId < b->clipId;
    });

    std::vector<SequenceEditor::ClipRow> displayClips;
    std::unordered_map<int32_t, const uapmd_app::ClipData*> clipLookup;
    clipLookup.reserve(clips.size());
    for (const auto* clip : clips) {
        clipLookup[clip->clipId] = clip;
    }
    const double sampleRate = std::max(1.0, static_cast<double>(appModel.sampleRate()));
    auto secondsToTimelineUnits = [](double seconds) -> int32_t {
        if (!std::isfinite(seconds)) {
            return 0;
        }
        seconds = std::max(0.0, seconds);
        const double maxSeconds = static_cast<double>(std::numeric_limits<int32_t>::max() - 1);
        if (seconds > maxSeconds) {
            seconds = maxSeconds;
        }
        return static_cast<int32_t>(std::llround(seconds));
    };

    for (auto* clip : clips) {
        SequenceEditor::ClipRow row;
        row.clipId = clip->clipId;
        row.anchorClipId = clip->anchorClipId;

        // Format anchor origin
        row.anchorOrigin = (clip->anchorOrigin == uapmd_app::AnchorOrigin::Start) ? "Start" : "End";

        // Format position display
        double positionSeconds = clip->anchorOffset.toSeconds(appModel.sampleRate());
        row.position = std::format("{:+.3f}s", positionSeconds);

        // Format duration (needed when End anchor is selected)
        double durationSeconds = static_cast<double>(clip->durationSamples) / appModel.sampleRate();
        row.duration = std::format("{:.3f}s", durationSeconds);

        // Set name and filename (extract just filename from path)
        row.name = clip->name.empty() ? std::format("Clip {}", clip->clipId) : clip->name;
        if (clip->filepath.empty()) {
            row.filename = "(no file)";
        } else {
            // Extract filename from full path
            size_t lastSlash = clip->filepath.find_last_of("/\\");
            row.filename = (lastSlash != std::string::npos)
                ? clip->filepath.substr(lastSlash + 1)
                : clip->filepath;
        }
        row.mimeType = "";
        auto absolutePosition = clip->getAbsolutePosition(clipLookup);
        double absoluteStartSeconds = static_cast<double>(absolutePosition.samples) / sampleRate;
        double durationSecondsExact = static_cast<double>(clip->durationSamples) / sampleRate;
        row.timelineStart = secondsToTimelineUnits(absoluteStartSeconds);
        int32_t durationUnits = std::max(1, secondsToTimelineUnits(durationSecondsExact));
        int64_t computedEnd = static_cast<int64_t>(row.timelineStart) + durationUnits;
        if (computedEnd > std::numeric_limits<int32_t>::max()) {
            computedEnd = std::numeric_limits<int32_t>::max();
        }
        row.timelineEnd = static_cast<int32_t>(computedEnd);

        displayClips.push_back(row);
    }

    sequenceEditor_.refreshClips(trackIndex, displayClips);
}

void MainWindow::addClipToTrack(int32_t trackIndex, const std::string& filepath) {
    // Open file dialog if filepath is empty
    std::string selectedFile = filepath;
    if (selectedFile.empty()) {
        auto selection = pfd::open_file(
            "Select Audio File",
            ".",
            { "Audio Files", "*.wav *.flac *.ogg",
              "WAV Files", "*.wav",
              "FLAC Files", "*.flac",
              "OGG Files", "*.ogg",
              "All Files", "*" }
        );

        if (selection.result().empty())
            return; // User cancelled

        selectedFile = selection.result()[0];
    }

    auto reader = uapmd::createAudioFileReaderFromPath(selectedFile);
    if (!reader) {
        pfd::message("Load Failed",
                    "Could not load audio file: " + selectedFile + "\nSupported formats: WAV, FLAC, OGG",
                    pfd::choice::ok,
                    pfd::icon::error);
        return;
    }

    // Add clip at timeline position 0
    uapmd_app::TimelinePosition position;
    position.samples = 0;
    position.beats = 0.0;

    auto& appModel = uapmd::AppModel::instance();
    auto result = appModel.addClipToTrack(trackIndex, position, std::move(reader), selectedFile);

    if (!result.success) {
        pfd::message("Add Clip Failed",
                    "Could not add clip to track: " + result.error,
                    pfd::choice::ok,
                    pfd::icon::error);
        return;
    }

    // Refresh the sequence editor display
    refreshSequenceEditorForTrack(trackIndex);
}

void MainWindow::removeClipFromTrack(int32_t trackIndex, int32_t clipId) {
    auto& appModel = uapmd::AppModel::instance();
    appModel.removeClipFromTrack(trackIndex, clipId);
    refreshSequenceEditorForTrack(trackIndex);
}

void MainWindow::clearAllClipsFromTrack(int32_t trackIndex) {
    auto& appModel = uapmd::AppModel::instance();
    auto tracks = appModel.getAppTracks();

    if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(tracks.size())) {
        return;
    }

    tracks[trackIndex]->clipManager().clearAll();
    refreshSequenceEditorForTrack(trackIndex);
}

void MainWindow::updateClip(int32_t trackIndex, int32_t clipId, int32_t anchorId, const std::string& origin, const std::string& position) {
    auto& appModel = uapmd::AppModel::instance();
    auto tracks = appModel.getAppTracks();

    if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(tracks.size())) {
        return;
    }

    // Parse position string (e.g., "+2.5s" or "-1.0s")
    double offsetSeconds = 0.0;
    try {
        // Remove trailing 's' if present
        std::string posStr = position;
        if (!posStr.empty() && posStr.back() == 's') {
            posStr = posStr.substr(0, posStr.length() - 1);
        }
        offsetSeconds = std::stod(posStr);
    } catch (const std::exception& e) {
        std::cerr << "Failed to parse position string: " << position << std::endl;
        return;
    }

    // Parse origin string
    uapmd_app::AnchorOrigin anchorOrigin = uapmd_app::AnchorOrigin::Start;
    if (origin == "End") {
        anchorOrigin = uapmd_app::AnchorOrigin::End;
    }

    // Convert to TimelinePosition
    uapmd_app::TimelinePosition anchorOffset = uapmd_app::TimelinePosition::fromSeconds(offsetSeconds, appModel.sampleRate());

    // Update the clip
    tracks[trackIndex]->clipManager().setClipAnchor(clipId, anchorId, anchorOrigin, anchorOffset);
    refreshSequenceEditorForTrack(trackIndex);
}

void MainWindow::updateClipName(int32_t trackIndex, int32_t clipId, const std::string& name) {
    auto& appModel = uapmd::AppModel::instance();
    auto tracks = appModel.getAppTracks();

    if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(tracks.size())) {
        return;
    }

    tracks[trackIndex]->clipManager().setClipName(clipId, name);
    refreshSequenceEditorForTrack(trackIndex);
}

void MainWindow::changeClipFile(int32_t trackIndex, int32_t clipId) {
    auto& appModel = uapmd::AppModel::instance();
    auto tracks = appModel.getAppTracks();

    if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(tracks.size())) {
        return;
    }

    // Open file dialog to select new audio file
    auto selection = pfd::open_file(
        "Select Audio File",
        ".",
        { "Audio Files", "*.wav *.flac *.ogg",
          "WAV Files", "*.wav",
          "FLAC Files", "*.flac",
          "OGG Files", "*.ogg",
          "All Files", "*" }
    );

    if (selection.result().empty())
        return; // User cancelled

    std::string selectedFile = selection.result()[0];

    // Create new audio file reader
    auto reader = uapmd::createAudioFileReaderFromPath(selectedFile);
    if (!reader) {
        pfd::message("Load Failed",
                    "Could not load audio file: " + selectedFile + "\nSupported formats: WAV, FLAC, OGG",
                    pfd::choice::ok,
                    pfd::icon::error);
        return;
    }

    // Get the clip to find its current source node ID
    auto* clip = tracks[trackIndex]->clipManager().getClip(clipId);
    if (!clip) {
        pfd::message("Error",
                    "Could not find clip",
                    pfd::choice::ok,
                    pfd::icon::error);
        return;
    }

    // Reuse the existing source node instance ID
    int32_t sourceNodeId = clip->sourceNodeInstanceId;

    // Create new source node with same instance ID
    auto sourceNode = std::make_unique<uapmd_app::AppAudioFileSourceNode>(
        sourceNodeId,
        std::move(reader)
    );

    // Get the duration from the new source node
    int64_t durationSamples = sourceNode->totalLength();

    // Replace the source node in the track
    if (!tracks[trackIndex]->replaceClipSourceNode(clipId, std::move(sourceNode))) {
        pfd::message("Replace Failed",
                    "Could not replace clip source node",
                    pfd::choice::ok,
                    pfd::icon::error);
        return;
    }

    // Update the filepath and duration in the clip data
    tracks[trackIndex]->clipManager().setClipFilepath(clipId, selectedFile);
    tracks[trackIndex]->clipManager().resizeClip(clipId, durationSamples);

    // Refresh the sequence editor display
    refreshSequenceEditorForTrack(trackIndex);
}

void MainWindow::moveClipAbsolute(int32_t trackIndex, int32_t clipId, double seconds) {
    auto& appModel = uapmd::AppModel::instance();
    auto tracks = appModel.getAppTracks();

    if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(tracks.size())) {
        return;
    }

    double sr = std::max(1.0, static_cast<double>(appModel.sampleRate()));
    uapmd_app::TimelinePosition newOffset = uapmd_app::TimelinePosition::fromSeconds(seconds, static_cast<int32_t>(sr));
    tracks[trackIndex]->clipManager().setClipAnchor(clipId, -1, uapmd_app::AnchorOrigin::Start, newOffset);
    refreshSequenceEditorForTrack(trackIndex);
}

}
