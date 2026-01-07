#pragma once

#include <midicci/midicci.hpp> // include before anything that indirectly includes X.h
#include <remidy-tooling/PluginScanTool.hpp>
#include <uapmd/uapmd.hpp>
#include <format>
#include <thread>
#include <string>
#include <unordered_map>
#include <optional>
#include <mutex>

namespace uapmd {
    // Forward declaration
    class AudioPluginSequencer;

    class TransportController {
        AudioPluginSequencer* sequencer_;
        std::string currentFile_;
        bool isPlaying_ = false;
        bool isPaused_ = false;
        bool isRecording_ = false;
        float playbackPosition_ = 0.0f;
        float playbackLength_ = 0.0f;
        float volume_ = 0.8f;

    public:
        explicit TransportController(AudioPluginSequencer* sequencer);

        // State getters
        const std::string& currentFile() const { return currentFile_; }
        bool isPlaying() const { return isPlaying_; }
        bool isPaused() const { return isPaused_; }
        bool isRecording() const { return isRecording_; }
        float playbackPosition() const { return playbackPosition_; }
        float playbackLength() const { return playbackLength_; }
        float volume() const { return volume_; }

        // State setters
        void setVolume(float volume) { volume_ = volume; }

        // Transport control methods
        void loadFile();
        void unloadFile();
        void play();
        void stop();
        void pause();
        void resume();
        void record();
    };

    class AppModel {
    public:
        // Plugin instance metadata
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

        // Device state containing MIDI device and associated plugin instances
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

        // Device entry wrapper for container management
        struct DeviceEntry {
            int id;
            std::shared_ptr<DeviceState> state;
        };

    private:
        AudioPluginSequencer sequencer_;
        remidy_tooling::PluginScanTool pluginScanTool_;
        std::unique_ptr<TransportController> transportController_;
        std::atomic<bool> isScanning_{false};
        mutable std::mutex devicesMutex_;
        std::vector<DeviceEntry> devices_;
        int32_t nextDeviceId_ = 1;

    public:
        static void instantiate();
        static AppModel& instance();
        static void cleanupInstance();
        AppModel(size_t audioBufferSizeInFrames, size_t umpBufferSizeInBytes, int32_t sampleRate, DeviceIODispatcher* dispatcher);

        AudioPluginSequencer& sequencer() { return sequencer_; }
        remidy_tooling::PluginScanTool& pluginScanTool() { return pluginScanTool_; }
        TransportController& transport() { return *transportController_; }
        bool isScanning() const { return isScanning_; }

        std::vector<std::function<void(bool success, std::string error)>> scanningCompleted{};

        // Configuration for creating a plugin instance with virtual MIDI device
        struct PluginInstanceConfig {
            std::string apiName = "default";
            std::string deviceName;  // Empty = auto-generate from plugin name
            std::string manufacturer = "UAPMD Project";
            std::string version = "0.1";
        };

        // Result from plugin instance creation
        struct PluginInstanceResult {
            int32_t instanceId = -1;
            std::shared_ptr<UapmdMidiDevice> device;
            int32_t trackIndex = -1;
            std::string pluginName;
            std::string error;
        };

        // Global callback registry - called whenever ANY instance is created (GUI or script)
        std::vector<std::function<void(const PluginInstanceResult&)>> instanceCreated{};

        // Global callback registry - called whenever ANY instance is removed (GUI or script)
        std::vector<std::function<void(int32_t instanceId)>> instanceRemoved{};

        // Result from device enable/disable operations
        struct DeviceStateResult {
            int32_t instanceId = -1;
            bool success = false;
            bool running = false;
            std::string statusMessage;
            std::string error;
        };

        // Global callback registry - called whenever a device is enabled
        std::vector<std::function<void(const DeviceStateResult&)>> deviceEnabled{};

        // Global callback registry - called whenever a device is disabled
        std::vector<std::function<void(const DeviceStateResult&)>> deviceDisabled{};

        // Create plugin instance with virtual MIDI device
        // Notifies all registered callbacks when complete
        void createPluginInstanceAsync(const std::string& format,
                                       const std::string& pluginId,
                                       int32_t trackIndex,
                                       const PluginInstanceConfig& config);

        // Remove plugin instance and its virtual MIDI device
        // Notifies all registered callbacks when complete
        void removePluginInstance(int32_t instanceId);

        // Enable virtual MIDI device for an instance
        // Notifies all registered callbacks when complete
        void enableUmpDevice(int32_t instanceId, const std::string& deviceName);

        // Disable virtual MIDI device for an instance
        // Notifies all registered callbacks when complete
        void disableUmpDevice(int32_t instanceId);

        // Query device state for display
        std::vector<DeviceEntry> getDevices() const;
        std::optional<std::shared_ptr<DeviceState>> getDeviceForInstance(int32_t instanceId) const;

        // Update device label from GUI
        void updateDeviceLabel(int32_t instanceId, const std::string& label);

        // Result from UI show/hide operations
        struct UIStateResult {
            int32_t instanceId = -1;
            bool success = false;
            bool visible = false;
            bool wasCreated = false;  // True if createUI() was called
            void* uiHandle = nullptr;  // UI window handle from createUI()
            std::string error;
        };

        // Global callback registry - called when a plugin UI is shown
        std::vector<std::function<void(const UIStateResult&)>> uiShown{};

        // Global callback registry - called when a plugin UI is hidden
        std::vector<std::function<void(const UIStateResult&)>> uiHidden{};

        // Global callback registry - called when user requests to show UI (from scripts)
        // MainWindow should handle this by preparing window and calling showPluginUI()
        std::vector<std::function<void(int32_t instanceId)>> uiShowRequested{};

        // Request to show plugin UI (from scripts) - triggers uiShowRequested callbacks
        void requestShowPluginUI(int32_t instanceId);

        // Show plugin UI - creates (if needsCreate) and shows the UI
        // Notifies all registered callbacks when complete
        void showPluginUI(int32_t instanceId, bool needsCreate, bool isFloating, void* parentHandle, std::function<bool(uint32_t, uint32_t)> resizeHandler);

        // Hide plugin UI
        // Notifies all registered callbacks when complete
        void hidePluginUI(int32_t instanceId);

        void performPluginScanning(bool forceRescan = false);
    };
}
