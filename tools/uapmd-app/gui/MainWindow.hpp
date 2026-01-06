#pragma once

#include <vector>
#include <string>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include <optional>
#include <imgui.h>
#include <uapmd/uapmd.hpp>
#include "MidiKeyboard.hpp"
#include "PluginList.hpp"
#include "ParameterList.hpp"
#include "TrackList.hpp"
#include "AudioDeviceSettings.hpp"
#include "ScriptEditor.hpp"
#include "SpectrumAnalyzer.hpp"
#include <remidy-gui/remidy-gui.hpp>
#include <PluginUIHelpers.hpp>

// Forward declarations for different window types
struct SDL_Window;
struct GLFWwindow;

namespace uapmd::gui {

struct GuiDefaults {
    std::string pluginName;
    std::string formatName;
    std::string apiName;
    std::string deviceName;
};

class MainWindow {
        bool isOpen_ = true;

        // Device settings
        AudioDeviceSettings audioDeviceSettings_;

        // Player settings
        std::string currentFile_;
        bool isPlaying_ = false;
        bool isPaused_ = false;
        bool isRecording_ = false;
        float playbackPosition_ = 0.0f;
        float playbackLength_ = 0.0f;
        float volume_ = 0.8f;
        bool showDeviceSettingsWindow_ = false;
        bool showPlayerSettingsWindow_ = false;

        // Spectrum analyzers
        SpectrumAnalyzer inputSpectrumAnalyzer_;
        SpectrumAnalyzer outputSpectrumAnalyzer_;

        // Instance control
        int selectedInstance_ = -1;
        std::vector<int32_t> instances_;
        TrackList trackList_;

        // Plugin selection
        bool showPluginSelectorWindow_ = false;
        PluginList pluginList_;

        // Script editor
        ScriptEditor scriptEditor_;

        // Plugin scanning
        bool forceRescan_ = false;

        // Track selection for plugin instantiation
        struct TrackDestinationOption {
            int32_t trackIndex;
            std::string label;
        };
        std::vector<TrackDestinationOption> trackOptions_;
        int selectedTrackOption_ = 0; // 0 = new track, 1+ = existing tracks
        char deviceNameInput_[128] = "";  // Empty by default, will use plugin name if not filled
        char apiInput_[64] = "default";

        // Virtual MIDI device state tracking
        struct PluginInstanceState {
            std::string pluginName;
            std::string pluginFormat;
            std::string pluginId;
            std::string statusMessage;
            bool instantiating = true;
            bool hasError = false;
            int32_t instanceId = -1;
            int32_t trackIndex = -1;
        };

        struct DeviceState {
            std::mutex mutex;
            std::shared_ptr<UapmdMidiDevice> device;
            std::string label;
            std::string apiName;
            std::string statusMessage;
            bool running = false;
            bool instantiating = false;
            bool hasError = false;
            std::unordered_map<int32_t, PluginInstanceState> pluginInstances;
        };

        struct DeviceEntry {
            int id;
            std::shared_ptr<DeviceState> state;
        };

        mutable std::mutex devicesMutex_;
        std::vector<DeviceEntry> devices_;
        int nextDeviceId_ = 1;

        std::unordered_map<int32_t, std::unique_ptr<remidy::gui::ContainerWindow>> pluginWindows_;
        std::unordered_map<int32_t, bool> pluginWindowEmbedded_;
        std::unordered_map<int32_t, bool> pluginWindowVisible_;  // Track which plugin UIs are currently visible
        std::unordered_map<int32_t, remidy::gui::Bounds> pluginWindowBounds_;
        std::vector<int32_t> pluginWindowsPendingClose_;
        std::unordered_set<int32_t> pluginWindowResizeIgnore_;

        ImGuiStyle baseStyle_{};
        float uiScale_ = 1.0f;
        bool uiScaleDirty_ = false;
        ImVec2 baseWindowSize_ = ImVec2(800.0f, 800.0f);
        ImVec2 lastWindowSize_ = ImVec2(0.0f, 0.0f);
        bool windowSizeRequestPending_ = false;
        bool waitingForWindowResize_ = false;
        ImVec2 requestedWindowSize_ = ImVec2(0.0f, 0.0f);
        std::vector<float> baseFontScales_;
        bool fontScalesCaptured_ = false;
        struct ChildWindowSizeState {
            ImVec2 baseSize{0.0f, 0.0f};
            ImVec2 lastSize{0.0f, 0.0f};
            bool waitingForResize = false;
        };
        std::unordered_map<std::string, ChildWindowSizeState> childWindowSizes_;

        // UMP device name buffers for each instance
        std::unordered_map<int32_t, std::array<char, 128>> umpDeviceNameBuffers_;

        struct DetailsWindowState {
            MidiKeyboard midiKeyboard;
            ParameterList parameterList;
            bool visible = false;
            std::vector<uapmd::PresetsMetadata> presets;
            int selectedPreset = -1;
            float pitchBendValue = 0.0f; // -1..1 UI range
            float channelPressureValue = 0.0f; // 0..1 UI range
        };

        std::unordered_map<int32_t, DetailsWindowState> detailsWindows_;

    public:
        explicit MainWindow(GuiDefaults defaults = {});
        void render(void* window);  // Generic window pointer
        void update();
        bool& isOpen() { return isOpen_; }
        bool consumePendingWindowResize(ImVec2& size);

    private:
        // No native embedding; we use dedicated windows for plugin UIs
        // Device settings
        void updateAudioDeviceSettingsData();
        void refreshDeviceList();
        void applyDeviceSettings();
        void handleAudioDeviceChange();

        // Player settings
        void renderPlayerSettings();
        void play();
        void stop();
        void pause();
        void resume();
        void record();
        void loadFile();
        void unloadFile();

        // Instance control
        void renderInstanceControl();
        void refreshInstances();
        void refreshParameters(int32_t instanceId, DetailsWindowState& state);
        void applyParameterUpdates(int32_t instanceId, DetailsWindowState& state);
        void refreshPresets(int32_t instanceId, DetailsWindowState& state);
        void loadSelectedPreset(int32_t instanceId, DetailsWindowState& state);
        void renderParameterControls(int32_t instanceId, DetailsWindowState& state);
        bool handlePluginResizeRequest(int32_t instanceId, uint32_t width, uint32_t height);
        void onPluginWindowResized(int32_t instanceId);
        void onPluginWindowClosed(int32_t instanceId);
        bool fetchPluginUISize(int32_t instanceId, uint32_t &width, uint32_t &height);

        // Details window management
        void showDetailsWindow(int32_t instanceId);
        void hideDetailsWindow(int32_t instanceId);
        void onDetailsWindowClosed(int32_t instanceId);
        void renderDetailsWindows();

        // State save/load
        void savePluginState(int32_t instanceId);
        void loadPluginState(int32_t instanceId);

        // Plugin selection
        void refreshPluginList();
        void renderPluginSelector();

        // Virtual MIDI device management
        void createDeviceForPlugin(const std::string& format, const std::string& pluginId, int32_t trackIndex);

        // TrackList helper methods
        void updateTrackListData();
        std::optional<TrackInstance> buildTrackInstanceInfo(int32_t instanceId);
        void handleShowUI(int32_t instanceId);
        void handleHideUI(int32_t instanceId);
        void handleEnableDevice(int32_t instanceId, const std::string& deviceName);
        void handleDisableDevice(int32_t instanceId);
        void handleRemoveInstance(int32_t instanceId);
        void sendPitchBend(int32_t instanceId, float normalizedValue);
        void sendChannelPressure(int32_t instanceId, float pressure);
        void renderDeviceSettingsWindow();
        void renderPlayerSettingsWindow();
        void renderPluginSelectorWindow();
        void applyUiScale(float scale);
        void requestWindowResize();
        void captureFontScales();
        void applyFontScaling();
        void setNextChildWindowSize(const std::string& id, ImVec2 defaultBaseSize);
        void updateChildWindowSizeState(const std::string& id);
    };
}
