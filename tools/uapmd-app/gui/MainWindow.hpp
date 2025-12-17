#pragma once

#include <vector>
#include <string>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <uapmd/uapmd.hpp>
#include "MidiKeyboard.hpp"
#include <remidy-gui/remidy-gui.hpp>
#include <PluginUIHelpers.hpp>

// Forward declarations for different window types
struct SDL_Window;
struct GLFWwindow;

namespace uapmd {
    class UapmdMidiDevice;
}

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
        std::vector<std::string> inputDevices_;
        std::vector<std::string> outputDevices_;
        int selectedInputDevice_ = 0;
        int selectedOutputDevice_ = 0;
        int bufferSize_ = 512;
        int sampleRate_ = 44100;

        // Player settings
        std::string currentFile_;
        std::vector<std::string> recentFiles_;
        bool isPlaying_ = false;
        bool isPaused_ = false;
        bool isRecording_ = false;
        float playbackPosition_ = 0.0f;
        float playbackLength_ = 0.0f;
        float volume_ = 0.8f;

        // Spectrum analyzer data
        static constexpr int kSpectrumBars = 32;
        float inputSpectrum_[kSpectrumBars] = {};
        float outputSpectrum_[kSpectrumBars] = {};

        // Instance control
        int selectedInstance_ = -1;
        std::vector<int32_t> instances_;

        // Plugin selection
        bool showPluginSelector_ = false;
        std::vector<PluginEntry> availablePlugins_;
        std::string selectedPluginFormat_;
        std::string selectedPluginId_;
        char searchFilter_[256] = "";

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

        // Virtual MIDI device state tracking (from uapmd-service)
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
        std::unordered_map<int32_t, remidy::gui::Bounds> pluginWindowBounds_;
        std::vector<int32_t> pluginWindowsPendingClose_;
        std::unordered_set<int32_t> pluginWindowResizeIgnore_;

        // UMP device name buffers for each instance
        std::unordered_map<int32_t, std::array<char, 128>> umpDeviceNameBuffers_;

        struct DetailsWindowState {
            MidiKeyboard midiKeyboard;
            bool visible = false;
            bool reflectEventOut = true;
            char parameterFilter[256] = "";
            std::vector<uapmd::ParameterMetadata> parameters;
            std::vector<float> parameterValues;
            std::vector<std::string> parameterValueStrings;
            std::vector<uapmd::PresetsMetadata> presets;
            int selectedPreset = -1;
        };

        std::unordered_map<int32_t, DetailsWindowState> detailsWindows_;

    public:
        explicit MainWindow(GuiDefaults defaults = {});
        void render(void* window);  // Generic window pointer
        void update();
        bool& isOpen() { return isOpen_; }

    private:
        // No native embedding; we use dedicated windows for plugin UIs
        // Device settings
        void renderDeviceSettings();
        void refreshDeviceList();
        void applyDeviceSettings();

        // Player settings
        void renderPlayerSettings();
        void play();
        void stop();
        void pause();
        void resume();
        void record();
        void loadFile();

        // Instance control
        void renderInstanceControl();
        void refreshInstances();
        void refreshParameters(int32_t instanceId, DetailsWindowState& state);
        void applyParameterUpdates(int32_t instanceId, DetailsWindowState& state);
        void refreshPresets(int32_t instanceId, DetailsWindowState& state);
        void loadSelectedPreset(int32_t instanceId, DetailsWindowState& state);
        void renderParameterControls(int32_t instanceId, DetailsWindowState& state);
        void updateParameterValueString(size_t parameterIndex, int32_t instanceId, DetailsWindowState& state);
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
        void renderVirtualMidiDeviceManager();
        void createDeviceForPlugin(const std::string& format, const std::string& pluginId, int32_t trackIndex);

    };
}
