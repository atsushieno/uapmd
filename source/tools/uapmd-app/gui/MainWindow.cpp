#include <cstring>
#include <algorithm>
#include <array>
#include <format>
#include <iostream>
#include <filesystem>
#include <optional>
#include <ranges>
#include <cmath>
#include <limits>

#include "PlatformDialogs.hpp"

#include <imgui.h>
#include "SharedTheme.hpp"
#include "FontLoader.hpp"

#include "MainWindow.hpp"

#include "FontIcons.hpp"
#include "../AppModel.hpp"
#include "../DocumentProviderHelpers.hpp"

namespace {
constexpr std::array<float, 7> kUiScaleOptions{0.5f, 0.8f, 1.0f, 1.2f, 1.5f, 2.0f, 4.0f};
constexpr std::array<const char*, 7> kUiScaleLabels{"x0.5", "x0.8", "x1.0", "x1.2", "x1.5", "x2.0", "x4.0"};

float snapScaleToOption(float scale) {
    float snapped = kUiScaleOptions.front();
    float bestDiff = std::numeric_limits<float>::max();
    for (float option : kUiScaleOptions) {
        float optionDiff = std::fabs(option - scale);
        if (optionDiff < bestDiff) {
            bestDiff = optionDiff;
            snapped = option;
        }
    }
    return snapped;
}
} // namespace

namespace uapmd::gui {

MainWindow::MainWindow(GuiDefaults defaults) {
    SetupImGuiStyle();
    // Font is already loaded in main_common.cpp before renderer initialization
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
    // Force the runtime audio device to match the UI defaults (sample rate, buffer) immediately.
    handleAudioDeviceChange();
    refreshInstances();
    refreshPluginList();

#ifdef UAPMD_HAS_MCP_SERVER
#ifdef __EMSCRIPTEN__
    // On Wasm, MCP is always active — auto-start immediately (no port/URL needed).
    mcpServer_ = std::make_unique<McpServer>(mcpPort_);
    mcpServer_->start();
#else
    if (defaults.mcpServerPort > 0) {
        mcpPort_ = defaults.mcpServerPort;
        mcpServer_ = std::make_unique<McpServer>(mcpPort_);
        mcpServer_->start();
    }
#endif
#endif

    // Register callback for when plugin scanning completes.
    // NOTE: this callback fires from the background scanning thread, so the UI
    // update must be dispatched to the main thread to avoid a data race with
    // render() reading availablePlugins_ at the same time.
    uapmd::AppModel::instance().scanningCompleted.push_back(
        [this](bool success, std::string error) {
            if (success) {
                remidy::EventLoop::runTaskOnMainThread([this]() {
                    refreshPluginList();
                    std::cout << "Plugin list refreshed after scanning" << std::endl;
                });
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
            timelineEditor_.instanceDetails().removeInstance(instanceId);

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
        timelineEditor_.instanceDetails().showWindow(instanceId);
        trackList_.markDirty();
    });

    trackList_.setOnHideDetails([this](int32_t instanceId) {
        timelineEditor_.instanceDetails().hideWindow(instanceId);
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

    // Track layout change notifications
    uapmd::AppModel::instance().trackLayoutChanged.push_back(
        [this](const uapmd::AppModel::TrackLayoutChange& change) {
            handleTrackLayoutChange(change);
        });

    // Set up AudioDeviceSettings callbacks
    audioDeviceSettings_.setOnDeviceChanged([this]() {
        handleAudioDeviceChange();
    });

    // Register device change listener with AudioIODeviceManager
    auto audioManager = uapmd::AudioIODeviceManager::instance();
    audioDeviceSettings_.setPlatformProvidesAutoBufferSize(audioManager->platformProvidesAutoBufferSize());
    audioDeviceSettings_.setUseAutoBufferSize(uapmd::AppModel::instance().autoBufferSizeEnabled());
    audioManager->setDeviceChangeCallback([this](int32_t deviceId, AudioIODeviceChange change) {
        // Refresh device list when devices are added or removed
        refreshDeviceList();
    });

    timelineEditor_.refreshAllSequenceEditorTracks();

    // Set up TimelineEditor callbacks
    timelineEditor_.setCallbacks({
        .buildTrackInstanceInfo = [this](int32_t instanceId) { return buildTrackInstanceInfo(instanceId); },
        .handleRemoveInstance = [this](int32_t instanceId) { handleRemoveInstance(instanceId); },
        .savePluginState = [this](int32_t instanceId) { savePluginState(instanceId); },
        .loadPluginState = [this](int32_t instanceId) { loadPluginState(instanceId); },
        .onInstanceDetailsClosed = [this](int32_t) { trackList_.markDirty(); },
        .showMixerMonitor = [this]() { showMixerMonitorWindow_ = !showMixerMonitorWindow_; },
        .showPluginInstances = [this]() { showAudioGraphWindow_ = !showAudioGraphWindow_; },
    });

    // Set up child window size helpers for TimelineEditor
    timelineEditor_.setChildWindowSizeHelper(
        [this](const std::string& id, ImVec2 size) { setNextChildWindowSize(id, size); },
        [this](const std::string& id) { updateChildWindowSizeState(id); }
    );

    exporterWindow_.setCallbacks({
        .setChildSize = [this](const std::string& id, ImVec2 size) { setNextChildWindowSize(id, size); },
        .updateChildSizeState = [this](const std::string& id) { updateChildWindowSizeState(id); }
    });

    audioImportWindow_.setCallbacks({
        .setChildSize = [this](const std::string& id, ImVec2 size) { setNextChildWindowSize(id, size); },
        .updateChildSizeState = [this](const std::string& id) { updateChildWindowSizeState(id); },
        .applyImportResult = [this](uapmd::import::AudioImportResult&& result) {
            timelineEditor_.applyAudioImportResult(std::move(result));
        }
    });
}

void MainWindow::render(void* window) {
#ifdef UAPMD_HAS_MCP_SERVER
    if (mcpServer_)
        mcpServer_->processMainThreadQueue();
#endif

    // Use the entire screen space as the main window (no nested window).
    // On platforms with system UI overlays (Android 15+, iOS notch/home indicator),
    // safeAreaInsets_ shrinks the content area so interactive elements stay reachable.
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 displaySize = io.DisplaySize;

    // Content area = display minus safe-area insets (zero on desktop)
    ImVec2 contentPos(safeAreaInsets_[0], safeAreaInsets_[1]);
    ImVec2 contentSize(
        displaySize.x - safeAreaInsets_[0] - safeAreaInsets_[2],
        displaySize.y - safeAreaInsets_[1] - safeAreaInsets_[3]);

    constexpr float kWindowSizeEpsilon = 1.0f;
    if (contentSize.x > 0.0f && contentSize.y > 0.0f) {
        if (lastWindowSize_.x == 0.0f && lastWindowSize_.y == 0.0f) {
            baseWindowSize_.x = contentSize.x / uiScale_;
            baseWindowSize_.y = contentSize.y / uiScale_;
        }

        const float deltaX = std::fabs(contentSize.x - lastWindowSize_.x);
        const float deltaY = std::fabs(contentSize.y - lastWindowSize_.y);

        if (waitingForWindowResize_) {
            const bool reachedTarget = std::fabs(contentSize.x - requestedWindowSize_.x) < kWindowSizeEpsilon &&
                                       std::fabs(contentSize.y - requestedWindowSize_.y) < kWindowSizeEpsilon;
            if (reachedTarget || (deltaX > kWindowSizeEpsilon || deltaY > kWindowSizeEpsilon)) {
                waitingForWindowResize_ = false;
                baseWindowSize_.x = contentSize.x / uiScale_;
                baseWindowSize_.y = contentSize.y / uiScale_;
            }
        } else if (deltaX > kWindowSizeEpsilon || deltaY > kWindowSizeEpsilon) {
            baseWindowSize_.x = contentSize.x / uiScale_;
            baseWindowSize_.y = contentSize.y / uiScale_;
        }

        lastWindowSize_ = contentSize;
    }
    ImGui::SetNextWindowPos(contentPos);
    ImGui::SetNextWindowSize(contentSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f * uiScale_, 8.0f * uiScale_));

    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

    if (ImGui::Begin("MainAppWindow", nullptr, window_flags)) {
        if (ImGui::BeginChild("MainToolbar", ImVec2(0, 90.0f * uiScale_), false, ImGuiWindowFlags_NoScrollbar)) {
            auto& appModel = uapmd::AppModel::instance();
            const bool audioEngineEnabled = appModel.isAudioEngineEnabled();
            const char* audioEngineLabel = audioEngineEnabled ? "Audio Engine: On" : "Audio Engine: Off";
            const ImVec4 onColor(0.25f, 0.58f, 0.33f, 1.0f);
            const ImVec4 onHoverColor(0.30f, 0.66f, 0.39f, 1.0f);
            const ImVec4 onActiveColor(0.20f, 0.50f, 0.26f, 1.0f);
            const ImVec4 offColor(0.58f, 0.27f, 0.21f, 1.0f);
            const ImVec4 offHoverColor(0.67f, 0.32f, 0.26f, 1.0f);
            const ImVec4 offActiveColor(0.48f, 0.20f, 0.19f, 1.0f);
            const ImVec4& baseColor = audioEngineEnabled ? onColor : offColor;
            const ImVec4& hoverColor = audioEngineEnabled ? onHoverColor : offHoverColor;
            const ImVec4& activeColor = audioEngineEnabled ? onActiveColor : offActiveColor;
            ImGui::PushStyleColor(ImGuiCol_Button, baseColor);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hoverColor);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, activeColor);
            if (ImGui::Button(audioEngineLabel)) {
                appModel.setAudioEngineEnabled(!audioEngineEnabled);
            }
            ImGui::PopStyleColor(3);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(audioEngineEnabled ? "Click to turn off the audio engine" : "Click to turn on the audio engine");
            }
            ImGui::SameLine();

            if (ImGui::Button("Device Settings")) {
                showDeviceSettingsWindow_ = !showDeviceSettingsWindow_;
            }
            ImGui::SameLine();

            // Transport controls
            auto& transport = appModel.transport();
            if (!audioEngineEnabled)
                ImGui::BeginDisabled();
            const char* playStopLabel = transport.isPlaying() ? icons::Stop : icons::Play;
            if (ImGui::Button(playStopLabel)) {
                if (transport.isPlaying())
                    transport.stop();
                else
                    transport.play();
            }
            ImGui::SameLine();

            if (!transport.isPlaying())
                ImGui::BeginDisabled();
            const char* pauseResumeLabel = transport.isPaused() ? icons::Play : icons::Pause;
            if (ImGui::Button(pauseResumeLabel)) {
                if (transport.isPaused())
                    transport.resume();
                else
                    transport.pause();
            }
            if (!transport.isPlaying())
                ImGui::EndDisabled();
            if (!audioEngineEnabled)
                ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Scale:");
            ImGui::SameLine();
            int currentScaleIndex = 0;
            for (size_t i = 0; i < kUiScaleOptions.size(); ++i) {
                if (std::fabs(uiScale_ - kUiScaleOptions[i]) < 0.001f) {
                    currentScaleIndex = static_cast<int>(i);
                    break;
                }
            }
            int selectedIndex = currentScaleIndex;
            ImGui::SetNextItemWidth(100.0f * uiScale_);
            if (ImGui::BeginCombo("##UiScaleCombo", kUiScaleLabels[currentScaleIndex])) {
                for (int i = 0; i < static_cast<int>(kUiScaleOptions.size()); ++i) {
                    bool isSelected = (selectedIndex == i);
                    if (ImGui::Selectable(kUiScaleLabels[i], isSelected)) {
                        selectedIndex = i;
                    }
                    if (isSelected) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
            if (selectedIndex != currentScaleIndex) {
                applyUiScale(kUiScaleOptions[selectedIndex]);
                requestWindowResize();
            }
            ImGui::SameLine();

            // Theme toggle
            const char* themeLabel = icons::LightDarkSwitch;
            if (ImGui::Button(themeLabel)) {
                toggleTheme();
            }

            if (ImGui::Button("Plugins")) {
                timelineEditor_.pluginSelector().setTargetNewTrack();
                auto& showSelector = timelineEditor_.showPluginSelectorWindow();
                showSelector = !showSelector;
            }
            ImGui::SameLine();
            if (ImGui::Button("Script")) {
                if (scriptEditor_.isOpen())
                    scriptEditor_.hide();
                else
                    scriptEditor_.show();
            }
            ImGui::SameLine();
#ifdef UAPMD_HAS_MCP_SERVER
            {
#ifdef __EMSCRIPTEN__
                // Wasm: MCP is always active as a C export — static green button.
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.55f, 0.28f, 1.0f));
                if (ImGui::Button("MCP"))
                    ImGui::OpenPopup("MCPSettings");
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("MCP active — call via Module.ccall('uapmd_mcp_call',...)");
                ImGui::SetNextWindowSizeConstraints(
                    ImVec2(280.0f * uiScale_, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
                if (ImGui::BeginPopup("MCPSettings")) {
                    ImGui::TextDisabled("MCP  (WebAssembly)");
                    ImGui::Separator();
                    ImGui::TextWrapped("MCP is always active as a JavaScript export.");
                    ImGui::Spacing();
                    ImGui::TextUnformatted("JSON-RPC call:");
                    ImGui::TextUnformatted("  Module.ccall(");
                    ImGui::TextUnformatted("    'uapmd_mcp_call',");
                    ImGui::TextUnformatted("    'string',['string'],[req])");
                    ImGui::Spacing();
                    ImGui::TextUnformatted("JS eval:");
                    ImGui::TextUnformatted("  Module.ccall(");
                    ImGui::TextUnformatted("    'uapmd_eval',");
                    ImGui::TextUnformatted("    'string',['string'],[code])");
                    ImGui::EndPopup();
                }
#else
                // Desktop / mobile: full connect/disconnect popover.
                // Button colour reflects connection state.
                const auto mcpState = mcpServer_
                    ? mcpServer_->connectionState()
                    : McpConnectionState::Idle;
                ImVec4 mcpColor;
                switch (mcpState) {
                case McpConnectionState::Connected:   mcpColor = ImVec4(0.20f, 0.55f, 0.28f, 1.0f); break;
                case McpConnectionState::Connecting:  mcpColor = ImVec4(0.55f, 0.50f, 0.10f, 1.0f); break;
                case McpConnectionState::Error:       mcpColor = ImVec4(0.55f, 0.20f, 0.20f, 1.0f); break;
                default:                              mcpColor = ImVec4(0.35f, 0.35f, 0.35f, 1.0f); break;
                }
                ImGui::PushStyleColor(ImGuiCol_Button, mcpColor);
                if (ImGui::Button("MCP"))
                    ImGui::OpenPopup("MCPSettings");
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) {
                    const auto tip = mcpServer_ ? mcpServer_->statusMessage() : std::string("Click to configure MCP");
                    ImGui::SetTooltip("%s", tip.c_str());
                }

                ImGui::SetNextWindowSizeConstraints(
                    ImVec2(280.0f * uiScale_, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
                if (ImGui::BeginPopup("MCPSettings")) {
                    ImGui::TextDisabled("MCP Settings");
                    ImGui::Separator();

#ifdef UAPMD_MCP_HAS_HTTP_SERVER
                    // Mode selector — only meaningful on desktop.
                    ImGui::Text("Mode:");
                    ImGui::SameLine();
                    ImGui::RadioButton("Server", &mcpMode_, 0);
                    ImGui::SameLine();
                    ImGui::RadioButton("Client", &mcpMode_, 1);
                    ImGui::Separator();
#endif

                    if (mcpMode_ == 0) {
#ifdef UAPMD_MCP_HAS_HTTP_SERVER
                        ImGui::InputInt("Port", &mcpPort_);
                        if (mcpPort_ < 1024)  mcpPort_ = 1024;
                        if (mcpPort_ > 65535) mcpPort_ = 65535;
#endif
                    } else {
                        ImGui::TextUnformatted("Relay URL");
                        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                        ImGui::InputText("##mcpRelayUrl", mcpRelayUrl_, sizeof(mcpRelayUrl_));
                        ImGui::Checkbox("Auto-reconnect", &mcpAutoReconnect_);
                    }

                    ImGui::Separator();
                    if (mcpServer_) {
                        const auto status = mcpServer_->statusMessage();
                        ImGui::TextUnformatted(status.c_str());
                    }

                    const bool running = mcpServer_ != nullptr;
                    if (ImGui::Button(running ? "Disconnect" : "Connect")) {
                        if (running) {
                            mcpServer_.reset();
                        } else {
                            if (mcpMode_ == 0) {
#ifdef UAPMD_MCP_HAS_HTTP_SERVER
                                mcpServer_ = std::make_unique<McpServer>(mcpPort_);
#endif
                            } else {
                                mcpServer_ = std::make_unique<McpServer>(
                                    std::string(mcpRelayUrl_), mcpAutoReconnect_);
                            }
                            if (mcpServer_)
                                mcpServer_->start();
                        }
                        ImGui::CloseCurrentPopup();
                    }

                    ImGui::EndPopup();
                }
#endif // __EMSCRIPTEN__
            }
            ImGui::SameLine();
#endif // UAPMD_HAS_MCP_SERVER
            bool openImportPopup = false;
            if (ImGui::Button("Import")) {
                openImportPopup = true;
            }
            if (openImportPopup)
                ImGui::OpenPopup("ImportActions");
            if (ImGui::BeginPopup("ImportActions")) {
                if (ImGui::MenuItem("Import MIDI Tracks (SMF)")) {
                    timelineEditor_.importMidiTracksWithPicker();
                    ImGui::CloseCurrentPopup();
                }
                if (ImGui::MenuItem("Import Split Audio Tracks (Demucs)")) {
                    audioImportWindow_.open();
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Project")) {
                ImGui::OpenPopup("ProjectActions");
            }
            if (ImGui::BeginPopup("ProjectActions")) {
                if (ImGui::MenuItem("Load Project")) {
                    handleLoadProject();
                }
                if (ImGui::MenuItem("Save Project")) {
                    handleSaveProject();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Render To File")) {
                    exporterWindow_.open();
                }
                ImGui::EndPopup();
            }
            ImGui::SameLine();

            // Spectrum analyzers - shrunken to half size
            ImVec2 spectrumSize = ImVec2(80.0f * uiScale_, 32.0f * uiScale_);
            inputSpectrumAnalyzer_.setSize(spectrumSize);
            outputSpectrumAnalyzer_.setSize(spectrumSize);

            ImGui::AlignTextToFramePadding();
            ImGui::Text("In");
            ImGui::SameLine();
            inputSpectrumAnalyzer_.render("##InputSpectrum");
            ImGui::SameLine();

            ImGui::AlignTextToFramePadding();
            ImGui::Text("Out");
            ImGui::SameLine();
            outputSpectrumAnalyzer_.render("##OutputSpectrum");
        }
        ImGui::EndChild();

        ImGui::Separator();
        timelineEditor_.render(uiScale_);
    }
    ImGui::End();
    ImGui::PopStyleVar(3);

    // Render floating windows
    timelineEditor_.renderPluginSelectorWindow(uiScale_);
    renderDeviceSettingsWindow();
    renderAudioGraphEditorWindow();
    renderMixerMonitorWindow();
    exporterWindow_.render(uiScale_);
    audioImportWindow_.render(uiScale_);

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

void MainWindow::renderAudioGraphEditorWindow() {
    if (!showAudioGraphWindow_) {
        return;
    }

    const std::string windowId = "AudioGraphEditor";
    setNextChildWindowSize(windowId, ImVec2(620.0f, 420.0f));
    if (ImGui::Begin("Audio Graph Editor", &showAudioGraphWindow_)) {
        updateChildWindowSizeState(windowId);
        trackList_.update();
        trackList_.render();
    }
    ImGui::End();
}

void MainWindow::renderMixerMonitorWindow() {
    if (!showMixerMonitorWindow_)
        return;

    const std::string windowId = "MixerMonitor";
    setNextChildWindowSize(windowId, ImVec2(900.0f, 420.0f));
    if (ImGui::Begin("Mixer Monitor", &showMixerMonitorWindow_)) {
        updateChildWindowSizeState(windowId);

        auto& appModel = uapmd::AppModel::instance();
        auto* engine = appModel.sequencer().engine();
        if (!engine) {
            ImGui::TextDisabled("Sequencer engine is not available.");
            ImGui::End();
            return;
        }

        const auto audiblePosition = engine->playbackPosition();
        const auto renderPosition = engine->renderPlaybackPosition();
        const bool playbackActive = engine->isPlaybackActive();
        const bool prerollActive = playbackActive && renderPosition < audiblePosition;
        const bool latencyDrainActive = !playbackActive && renderPosition > audiblePosition;

        ImGui::Text("Audible Position: %lld samples", static_cast<long long>(audiblePosition));
        ImGui::SameLine();
        ImGui::Text("Render Position: %lld samples", static_cast<long long>(renderPosition));
        ImGui::Text("Playback: %s", playbackActive ? "active" : "idle");
        ImGui::SameLine();
        ImGui::Text("Preroll: %s", prerollActive ? "active" : "inactive");
        ImGui::SameLine();
        ImGui::Text("Latency Drain: %s", latencyDrainActive ? "active" : "inactive");

        ImGui::Separator();

        if (ImGui::BeginTable("MixerMonitorTable", 7, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY)) {
            ImGui::TableSetupColumn("Track", ImGuiTableColumnFlags_WidthFixed, 130.0f * uiScale_);
            ImGui::TableSetupColumn("Plugins", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("Main Latency", ImGuiTableColumnFlags_WidthFixed, 120.0f * uiScale_);
            ImGui::TableSetupColumn("Render Lead", ImGuiTableColumnFlags_WidthFixed, 120.0f * uiScale_);
            ImGui::TableSetupColumn("Tail", ImGuiTableColumnFlags_WidthFixed, 110.0f * uiScale_);
            ImGui::TableSetupColumn("Out Buses", ImGuiTableColumnFlags_WidthFixed, 80.0f * uiScale_);
            ImGui::TableSetupColumn("Per-Bus Latency", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableHeadersRow();

            auto renderTrackRow = [engine, this](const char* label, uapmd::SequencerTrack* track, uint32_t mainLatency, uint32_t renderLead, double tailSeconds) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(label);

                ImGui::TableSetColumnIndex(1);
                std::string pluginSummary;
                if (track) {
                    const auto& ids = track->orderedInstanceIds();
                    for (size_t i = 0; i < ids.size(); ++i) {
                        auto* instance = engine->getPluginInstance(ids[i]);
                        if (!instance)
                            continue;
                        if (!pluginSummary.empty())
                            pluginSummary += " -> ";
                        pluginSummary += instance->displayName();
                    }
                }
                if (pluginSummary.empty())
                    pluginSummary = "(none)";
                ImGui::TextWrapped("%s", pluginSummary.c_str());

                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%u samples", mainLatency);

                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%u samples", renderLead);

                ImGui::TableSetColumnIndex(4);
                if (std::isinf(tailSeconds))
                    ImGui::TextUnformatted("infinite");
                else
                    ImGui::Text("%.3f s", tailSeconds);

                ImGui::TableSetColumnIndex(5);
                const uint32_t outputBusCount = track ? track->graph().outputBusCount() : 0;
                ImGui::Text("%u", outputBusCount);

                ImGui::TableSetColumnIndex(6);
                std::string busLatencySummary;
                if (track) {
                    for (uint32_t busIndex = 0; busIndex < outputBusCount; ++busIndex) {
                        if (!busLatencySummary.empty())
                            busLatencySummary += ", ";
                        busLatencySummary += std::format("bus {}: {}", busIndex, track->graph().outputLatencyInSamples(busIndex));
                    }
                }
                if (busLatencySummary.empty())
                    busLatencySummary = "-";
                ImGui::TextWrapped("%s", busLatencySummary.c_str());
            };

            renderTrackRow("Master Track",
                           engine->masterTrack(),
                           engine->masterTrackLatencyInSamples(),
                           engine->masterTrackRenderLeadInSamples(),
                           engine->masterTrack() ? engine->masterTrack()->tailLengthInSeconds() : 0.0);

            const auto& tracks = engine->tracks();
            for (size_t trackIndex = 0; trackIndex < tracks.size(); ++trackIndex) {
                auto* track = tracks[trackIndex];
                const std::string label = std::format("Track {}", trackIndex);
                renderTrackRow(label.c_str(),
                               track,
                               engine->trackLatencyInSamples(static_cast<uapmd_track_index_t>(trackIndex)),
                               engine->trackRenderLeadInSamples(static_cast<uapmd_track_index_t>(trackIndex)),
                               track ? track->tailLengthInSeconds() : 0.0);
            }

            ImGui::EndTable();
        }
    }
    ImGui::End();
}


void MainWindow::handleTrackLayoutChange(const uapmd::AppModel::TrackLayoutChange& change) {
    timelineEditor_.handleTrackLayoutChange(change);
    trackList_.markDirty();
}
void MainWindow::update() {
    if (auto* provider = uapmd::AppModel::instance().documentProvider())
        provider->tick();
}

void MainWindow::applySystemUiScale(float scale) {
    if (scale <= 0.0f)
        return;
    const float clamped = std::clamp(scale, 0.5f, 4.0f);
    const float snapped = snapScaleToOption(clamped);
    if (std::fabs(snapped - uiScale_) < 0.01f)
        return;
    applyUiScale(snapped);
    requestWindowResize();
}

void MainWindow::setSafeAreaInsets(const float insets[4]) {
    safeAreaInsets_[0] = insets[0];
    safeAreaInsets_[1] = insets[1];
    safeAreaInsets_[2] = insets[2];
    safeAreaInsets_[3] = insets[3];
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
    // Route the close request through AppModel so that all hide callbacks run
    // and our ContainerWindow visibility state stays in sync with the overlay.
    uapmd::AppModel::instance().hidePluginUI(instanceId);
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
    audioDeviceSettings_.setUseAutoBufferSize(uapmd::AppModel::instance().autoBufferSizeEnabled());

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

    // Let the driver constrain the buffer size list (e.g. Web Audio requires multiples of 128).
    auto availBufSizes = manager->getAvailableBufferSizes();
    if (!availBufSizes.empty())
        audioDeviceSettings_.setAvailableBufferSizes(availBufSizes);
}


std::optional<TrackInstance> MainWindow::buildTrackInstanceInfo(int32_t instanceId) {
    auto& sequencer = uapmd::AppModel::instance().sequencer();
    auto* instance = sequencer.engine()->getPluginInstance(instanceId);
    if (!instance) {
        return std::nullopt;
    }

    int32_t trackIndex = sequencer.engine()->findTrackIndexForInstance(instanceId);
    std::string pluginName = instance->displayName();
    std::string pluginFormat = instance->formatName();

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
        deviceSupported = uapmd::midiApiSupportsDynamicUmpEndpoints((*deviceState)->apiName);
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
    ti.detailsVisible = timelineEditor_.instanceDetails().isVisible(instanceId);
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

    const bool useAutoBuffer = audioDeviceSettings_.isAutoBufferSizeEnabled();
    uapmd::AppModel::instance().setAutoBufferSizeEnabled(useAutoBuffer);

    // Get selected buffer size (ignored when auto buffer mode is enabled)
    uint32_t bufferSize = useAutoBuffer
        ? 0u
        : static_cast<uint32_t>(audioDeviceSettings_.getBufferSize());

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
        uapmd::AppModel::instance().updateAudioDeviceSettings(static_cast<int32_t>(sampleRate), bufferSize);
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
    auto instances = sequencer.engine()->pluginHost()->instanceIds();

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
    timelineEditor_.instanceDetails().pruneInvalidInstances(instances);
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

    auto& scanTool = uapmd::AppModel::instance().pluginScanTool();
    auto rawEntries = scanTool.catalog().getPlugins();
    plugins.reserve(rawEntries.size());
    for (auto* plugin : rawEntries) {
        plugins.push_back({
            .format = plugin->format(),
            .id = plugin->pluginId(),
            .name = plugin->displayName(),
            .vendor = plugin->vendorName()
        });
    }

    timelineEditor_.pluginSelector().setPlugins(plugins);
}

void MainWindow::savePluginState(int32_t instanceId) {
    auto& sequencer = uapmd::AppModel::instance().sequencer();
    auto instance = sequencer.engine()->getPluginInstance(instanceId);

    std::string defaultFilename = std::format("{}.{}.state",
                                              instance->displayName(),
                                              instance->formatName());
    std::ranges::replace(defaultFilename, ' ', '_');

    std::vector<uapmd::DocumentFilter> filters{
        {"Plugin State Files", {}, {"*.state"}},
        {"All Files", {}, {"*"}}
    };

    uapmd::AppModel::instance().documentProvider()->pickSaveDocument(
        defaultFilename,
        filters,
        [instanceId](uapmd::DocumentPickResult pickResult) {
            if (!pickResult.success || pickResult.handles.empty())
                return;
            uapmd::AppModel::instance().savePluginState(
                instanceId, pickResult.handles[0],
                [](uapmd::AppModel::PluginStateResult result) {
                    if (!result.success)
                        platformError("Save Failed",
                                      std::format("Failed to save plugin state:\n{}", result.error));
                });
        }
    );
}

void MainWindow::loadPluginState(int32_t instanceId) {
    std::vector<uapmd::DocumentFilter> filters{
        {"Plugin State Files", {}, {"*.state"}},
        {"All Files", {}, {"*"}}
    };

    uapmd::AppModel::instance().documentProvider()->pickOpenDocuments(
        filters,
        false,
        [this, instanceId](uapmd::DocumentPickResult pickResult) {
            if (!pickResult.success || pickResult.handles.empty())
                return;
            uapmd::AppModel::instance().loadPluginState(
                instanceId, pickResult.handles[0],
                [this, instanceId](uapmd::AppModel::PluginStateResult result) {
                    if (!result.success) {
                        platformError("Load Failed",
                                      std::format("Failed to load plugin state:\n{}", result.error));
                        return;
                    }
                    timelineEditor_.instanceDetails().refreshParametersForInstance(instanceId);
                });
        }
    );
}

void MainWindow::createPluginInstance(const std::string& format, const std::string& pluginId, int32_t trackIndex) {
    // Prepare configuration
    uapmd::AppModel::PluginInstanceConfig config;
    config.apiName = std::string(timelineEditor_.pluginSelector().getApiInput());
    if (config.apiName.empty()) {
        config.apiName = "default";
    }
    config.deviceName = std::string(timelineEditor_.pluginSelector().getDeviceNameInput());  // Empty = auto-generate

    // Use AppModel's unified creation method with a completion callback
    // to show details window for GUI-initiated creation
    uapmd::AppModel::instance().createPluginInstanceAsync(format, pluginId, trackIndex, config,
        [this](const uapmd::AppModel::PluginInstanceResult& result) {
            // Only show details on successful creation
            if (result.error.empty() && result.instanceId >= 0) {
                timelineEditor_.instanceDetails().showWindow(result.instanceId);
            }
        });
}


void MainWindow::handleShowUI(int32_t instanceId) {
    auto& sequencer = uapmd::AppModel::instance().sequencer();
    auto* instance = sequencer.engine()->getPluginInstance(instanceId);
    if (!instance) return;

    std::string pluginName = instance->displayName();
    std::string pluginFormat = instance->formatName();

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
void MainWindow::handleSaveProject() {
    std::vector<uapmd::DocumentFilter> filters{
        {"UAPMD Project Archive", {}, {"*.uapmdz"}},
        {"All Files", {}, {"*"}}
    };

    uapmd::AppModel::instance().documentProvider()->pickSaveDocument(
        "project.uapmdz",
        filters,
        [](uapmd::DocumentPickResult pickResult) {
            if (!pickResult.success || pickResult.handles.empty())
                return;

            auto handle = pickResult.handles[0];
            std::string label = handle.display_name.empty() ? handle.id : handle.display_name;
            if (label.empty())
                label = "selected location";

            uapmd::AppModel::instance().saveProjectToDocument(
                std::move(handle),
                [label = std::move(label)](uapmd::DocumentIOResult ioResult) {
                    if (!ioResult.success) {
                        platformError("Save Failed", ioResult.error);
                    }
                });
        }
    );
}

void MainWindow::handleLoadProject() {
    std::vector<uapmd::DocumentFilter> filters{
        {"UAPMD Project Archive", {}, {"*.uapmdz"}},
        {"Legacy UAPMD Project", {}, {"*.uapmd"}},
        {"All Files", {}, {"*"}}
    };

    uapmd::AppModel::instance().documentProvider()->pickOpenDocuments(
        filters,
        false,
        [this](uapmd::DocumentPickResult pickResult) {
            if (!pickResult.success || pickResult.handles.empty())
                return;
            resolveDocumentHandle(
                pickResult.handles[0],
                [this](const std::filesystem::path& projectPath) {
                    auto& appModel = uapmd::AppModel::instance();
                    const bool wasEnabled = appModel.isAudioEngineEnabled();
                    if (wasEnabled)
                        appModel.setAudioEngineEnabled(false);
                    auto result = appModel.loadProjectFromResolvedPath(projectPath);
                    if (wasEnabled)
                        appModel.setAudioEngineEnabled(true);
                    if (!result.success) {
                        platformError("Load Failed", result.error);
                        return;
                    }
                    timelineEditor_.refreshAllSequenceEditorTracks();
                    timelineEditor_.invalidateMasterTrackSnapshot();
                    trackList_.markDirty();
                },
                [](const std::string& error) {
                    platformError("Load Failed", error);
                });
        }
    );
}

void MainWindow::toggleTheme() {
    currentTheme_ = (currentTheme_ == ThemeMode::Dark) ? ThemeMode::Light : ThemeMode::Dark;
    applyTheme(currentTheme_);
}

void MainWindow::applyTheme(ThemeMode mode) {
    currentTheme_ = mode;
    SetupImGuiStyle(mode);
    baseStyle_ = ImGui::GetStyle();
    applyUiScale(uiScale_);
}

}  // namespace uapmd::gui
