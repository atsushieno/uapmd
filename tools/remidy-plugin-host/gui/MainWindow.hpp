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

namespace uapmd::gui {
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
        bool isRecording_ = false;
        float playbackPosition_ = 0.0f;
        float playbackLength_ = 0.0f;
        float volume_ = 0.8f;

        // Instance control
        int selectedInstance_ = -1;
        std::vector<int32_t> instances_;
        std::vector<uapmd::ParameterMetadata> parameters_;
        std::vector<float> parameterValues_; // Store current parameter values
        std::vector<std::string> parameterValueStrings_; // Cache value strings to avoid calling valueToString() every frame
        std::vector<uapmd::PresetsMetadata> presets_;
        int selectedPreset_ = -1;

        // Plugin selection
        bool showPluginSelector_ = false;
        std::vector<PluginEntry> availablePlugins_;
        std::string selectedPluginFormat_;
        std::string selectedPluginId_;
        char searchFilter_[256] = "";
        char parameterFilter_[256] = "";

        // Plugin scanning
        bool forceRescan_ = false;

        // MIDI keyboard
        MidiKeyboard midiKeyboard_;

        std::unordered_map<int32_t, std::unique_ptr<remidy::gui::ContainerWindow>> pluginWindows_;
        std::unordered_map<int32_t, bool> pluginWindowEmbedded_;
        std::unordered_map<int32_t, remidy::gui::Bounds> pluginWindowBounds_;
        std::vector<int32_t> pluginWindowsPendingClose_;
        std::unordered_set<int32_t> pluginWindowResizeIgnore_;

    public:
        MainWindow();
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
        void playPause();
        void stop();
        void record();
        void loadFile();

        // Instance control
        void renderInstanceControl();
        void refreshInstances();
        void refreshParameters();
        void refreshPresets();
        void loadSelectedPreset();
        void renderParameterControls();
        void updateParameterValueString(size_t parameterIndex);
        bool handlePluginResizeRequest(int32_t instanceId, uint32_t width, uint32_t height);
        void onPluginWindowResized(int32_t instanceId);
        void onPluginWindowClosed(int32_t instanceId);
        bool fetchPluginUISize(int32_t instanceId, uint32_t &width, uint32_t &height);

        // Plugin selection
        void refreshPluginList();
        void renderPluginSelector();

    };
}
