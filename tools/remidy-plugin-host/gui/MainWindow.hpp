#pragma once

#include <vector>
#include <string>
#include <uapmd/uapmd.hpp>
#include <GLFW/glfw3.h>
#include "MidiKeyboard.hpp"

namespace uapmd::gui {
    struct PluginEntry {
        std::string format;
        std::string id;
        std::string name;
        std::string vendor;
    };
}

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
        std::vector<uapmd::PresetsMetadata> presets_;
        int selectedPreset_ = -1;

        // Plugin selection
        bool showPluginSelector_ = false;
        std::vector<uapmd::gui::PluginEntry> availablePlugins_;
        std::string selectedPluginFormat_;
        std::string selectedPluginId_;
        char searchFilter_[256] = "";

        // MIDI keyboard
        MidiKeyboard midiKeyboard_;

    public:
        MainWindow();
        void render(GLFWwindow* window);
        void update();
        bool& isOpen() { return isOpen_; }

    private:
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

        // Plugin selection
        void refreshPluginList();
        void renderPluginSelector();

    };
}