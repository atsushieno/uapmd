
#include "AppModel.hpp"
#include <iostream>

#define DEFAULT_AUDIO_BUFFER_SIZE 1024
#define DEFAULT_UMP_BUFFER_SIZE 65536
#define DEFAULT_SAMPLE_RATE 48000

std::unique_ptr<uapmd::AppModel> model{};

void uapmd::AppModel::instantiate() {
    model = std::make_unique<uapmd::AppModel>(DEFAULT_AUDIO_BUFFER_SIZE, DEFAULT_UMP_BUFFER_SIZE, DEFAULT_SAMPLE_RATE, defaultDeviceIODispatcher());
}

uapmd::AppModel& uapmd::AppModel::instance() {
    return *model;
}

void uapmd::AppModel::cleanupInstance() {
    model.reset();
}

uapmd::AppModel::AppModel(size_t audioBufferSizeInFrames, size_t umpBufferSizeInBytes, int32_t sampleRate, DeviceIODispatcher* dispatcher) :
        sequencer_(audioBufferSizeInFrames, umpBufferSizeInBytes, sampleRate, dispatcher),
        deviceController_(std::make_unique<VirtualMidiDeviceController>(&sequencer_)) {
}

void uapmd::AppModel::performPluginScanning(bool forceRescan) {
    if (isScanning_) {
        std::cout << "Plugin scanning already in progress" << std::endl;
        return;
    }

    isScanning_ = true;
    std::cout << "Starting plugin scanning (forceRescan: " << forceRescan << ")" << std::endl;

    // Run scanning in a separate thread to avoid blocking the UI
    std::thread scanningThread([this, forceRescan]() {
        try {
            static std::filesystem::path emptyPath{};
            int result;

            if (forceRescan) {
                // Force rescan - ignore existing cache
                result = pluginScanTool_.performPluginScanning(emptyPath);
            } else {
                // Normal scan - use existing cache if available
                result = pluginScanTool_.performPluginScanning();
            }

            // Save the updated cache
            pluginScanTool_.savePluginListCache();

            // Now trigger the sequencer to reload its catalog from the updated cache
            // This ensures the sequencer gets the new scan results
            sequencer_.performPluginScanning(false); // Load from cache, don't rescan

            bool success = (result == 0);
            std::string errorMsg = success ? "" : "Plugin scanning failed with error code " + std::to_string(result);

            std::cout << "Plugin scanning completed " << (success ? "successfully" : "with errors") << std::endl;

            // Notify callbacks on completion - the UI will refresh from the sequencer's updated catalog
            for (auto& callback : scanningCompleted) {
                callback(success, errorMsg);
            }

            isScanning_ = false;
        } catch (const std::exception& e) {
            std::cout << "Plugin scanning failed with exception: " << e.what() << std::endl;

            // Notify callbacks of failure
            for (auto& callback : scanningCompleted) {
                callback(false, std::string("Exception during scanning: ") + e.what());
            }

            isScanning_ = false;
        }
    });

    scanningThread.detach();
}

void uapmd::AppModel::createPluginInstanceAsync(const std::string& format,
                                                 const std::string& pluginId,
                                                 int32_t trackIndex,
                                                 const PluginInstanceConfig& config) {
    // Get plugin name from catalog
    std::string pluginName;
    auto& catalog = sequencer_.catalog();
    auto plugins = catalog.getPlugins();
    for (const auto& plugin : plugins) {
        if (plugin->format() == format && plugin->pluginId() == pluginId) {
            pluginName = plugin->displayName();
            break;
        }
    }

    if (pluginName.empty()) {
        pluginName = "Unknown Plugin";
    }

    // Determine device name
    std::string deviceLabel = config.deviceName.empty()
        ? std::format("{} [{}]", pluginName, format)
        : config.deviceName;

    // This is the same logic as VirtualMidiDeviceController::createDevice
    // but we call the callback instead of managing state
    std::string formatCopy = format;
    std::string pluginIdCopy = pluginId;

    auto instantiateCallback = [this, config, deviceLabel, pluginName](int32_t instanceId, std::string error) {
        PluginInstanceResult result;
        result.instanceId = instanceId;
        result.pluginName = pluginName;
        result.error = std::move(error);

        if (!result.error.empty() || instanceId < 0) {
            // Notify all registered callbacks
            for (auto& cb : instanceCreated) {
                cb(result);
            }
            return;
        }

        // Create virtual MIDI device
        auto actualTrackIndex = sequencer_.findTrackIndexForInstance(instanceId);
        auto midiDevice = createLibreMidiIODevice(config.apiName, deviceLabel, config.manufacturer, config.version);

        auto device = std::make_shared<UapmdMidiDevice>(midiDevice,
                                                         &sequencer_,
                                                         instanceId,
                                                         actualTrackIndex,
                                                         config.apiName,
                                                         deviceLabel,
                                                         config.manufacturer,
                                                         config.version);

        if (auto group = sequencer_.pluginGroup(instanceId); group.has_value()) {
            device->group(group.value());
        }

        sequencer_.assignMidiDeviceToPlugin(instanceId, midiDevice);
        device->initialize();

        result.device = device;
        result.trackIndex = actualTrackIndex;

        // Notify all registered callbacks
        for (auto& cb : instanceCreated) {
            cb(result);
        }
    };

    if (trackIndex < 0) {
        sequencer_.addSimplePluginTrack(formatCopy, pluginIdCopy, instantiateCallback);
    } else {
        sequencer_.addPluginToTrack(trackIndex, formatCopy, pluginIdCopy, instantiateCallback);
    }
}
