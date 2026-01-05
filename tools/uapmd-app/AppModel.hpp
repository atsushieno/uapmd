#pragma once

#include <midicci/midicci.hpp> // include before anything that indirectly includes X.h
#include <remidy-tooling/PluginScanTool.hpp>
#include <uapmd/uapmd.hpp>
#include "Controller/VirtualMidiDeviceController.hpp"
#include <format>
#include <thread>

namespace uapmd {
    class AppModel {
        AudioPluginSequencer sequencer_;
        remidy_tooling::PluginScanTool pluginScanTool_;
        std::unique_ptr<VirtualMidiDeviceController> deviceController_;
        std::atomic<bool> isScanning_{false};

    public:
        static void instantiate();
        static AppModel& instance();
        static void cleanupInstance();
        AppModel(size_t audioBufferSizeInFrames, size_t umpBufferSizeInBytes, int32_t sampleRate, DeviceIODispatcher* dispatcher);

        AudioPluginSequencer& sequencer() { return sequencer_; }
        remidy_tooling::PluginScanTool& pluginScanTool() { return pluginScanTool_; }
        VirtualMidiDeviceController* deviceController() { return deviceController_.get(); }
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

        // Create plugin instance with virtual MIDI device
        // Notifies all registered callbacks when complete
        void createPluginInstanceAsync(const std::string& format,
                                       const std::string& pluginId,
                                       int32_t trackIndex,
                                       const PluginInstanceConfig& config);

        // Remove plugin instance and its virtual MIDI device
        // Notifies all registered callbacks when complete
        void removePluginInstance(int32_t instanceId);

        void performPluginScanning(bool forceRescan = false);
    };
}
