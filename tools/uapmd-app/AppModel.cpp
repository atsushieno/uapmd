
#include "AppModel.hpp"
#include "uapmd/priv/audio/AudioFileFactory.hpp"
#include <portable-file-dialogs.h>
#include <iostream>
#include <fstream>

#define DEFAULT_AUDIO_BUFFER_SIZE 1024
#define DEFAULT_UMP_BUFFER_SIZE 65536
#define DEFAULT_SAMPLE_RATE 48000

std::unique_ptr<uapmd::AppModel> model{};

uapmd::TransportController::TransportController(AudioPluginSequencer* sequencer)
    : sequencer_(sequencer) {
}

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
        transportController_(std::make_unique<TransportController>(&sequencer_)) {
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
            sequencer_.engine()->performPluginScanning(false); // Load from cache, don't rescan

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
    auto& catalog = sequencer_.engine()->catalog();
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

    auto instantiateCallback = [this, config, deviceLabel, pluginName, format, pluginId](int32_t instanceId, int32_t trackId, std::string error) {
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

        auto actualTrackIndex = sequencer_.findTrackIndexForInstance(instanceId);

        // Create DeviceState and add to devices_ vector
        auto state = std::make_shared<DeviceState>();
        state->label = deviceLabel;
        state->apiName = config.apiName;
        state->instantiating = false;

        auto& pluginNode = state->pluginInstances[instanceId];
        pluginNode.instanceId = instanceId;
        pluginNode.pluginName = pluginName;
        pluginNode.pluginFormat = format;
        pluginNode.pluginId = pluginId;
        pluginNode.statusMessage = std::format("Plugin ready (instance {})", instanceId);
        pluginNode.instantiating = false;
        pluginNode.hasError = false;
        pluginNode.trackIndex = actualTrackIndex;

        {
            std::lock_guard lock(devicesMutex_);
            devices_.push_back(DeviceEntry{nextDeviceId_++, state});
        }

        // Reuse the dedicated logic for MIDI device initialization
        enableUmpDevice(instanceId, deviceLabel);

        result.device = state->device;
        result.trackIndex = actualTrackIndex;

        // Notify all registered callbacks
        for (auto& cb : instanceCreated) {
            cb(result);
        }
    };

    if (trackIndex < 0) {
        sequencer_.engine()->addSimpleTrack(formatCopy, pluginIdCopy, instantiateCallback);
    } else {
        sequencer_.engine()->addPluginToTrack(trackIndex, formatCopy, pluginIdCopy, instantiateCallback);
    }
}

void uapmd::AppModel::removePluginInstance(int32_t instanceId) {
    // Hide and destroy plugin UI before removing the instance
    auto* instance = sequencer_.engine()->getPluginInstance(instanceId);
    if (instance) {
        if (instance->hasUISupport() && instance->isUIVisible()) {
            instance->hideUI();
        }
        instance->destroyUI();
    }

    // Stop and remove virtual MIDI device if it exists
    {
        std::lock_guard lock(devicesMutex_);
        for (auto it = devices_.begin(); it != devices_.end(); ++it) {
            auto state = it->state;
            if (state) {
                std::lock_guard guard(state->mutex);
                if (state->pluginInstances.count(instanceId) > 0) {
                    if (state->device)
                        state->device->stop();
                    devices_.erase(it);
                    break;
                }
            }
        }
    }

    // Remove the plugin instance from sequencer
    sequencer_.removePluginInstance(instanceId);

    // Notify all registered callbacks
    for (auto& cb : instanceRemoved) {
        cb(instanceId);
    }
}

void uapmd::AppModel::enableUmpDevice(int32_t instanceId, const std::string& deviceName) {
    DeviceStateResult result;
    result.instanceId = instanceId;

    // Find the device for this instance
    std::shared_ptr<DeviceState> deviceState;
    {
        std::lock_guard lock(devicesMutex_);
        for (auto& entry : devices_) {
            auto state = entry.state;
            if (state && state->pluginInstances.count(instanceId) > 0) {
                deviceState = state;
                break;
            }
        }
    }

    if (!deviceState) {
        result.success = false;
        result.error = "Device state not found for instance";
        result.statusMessage = "Error";
        for (auto& cb : deviceEnabled) {
            cb(result);
        }
        return;
    }

    // Lock the device state for modifications
    std::lock_guard guard(deviceState->mutex);

    // If device was destroyed (disabled), recreate it
    if (!deviceState->device) {
        auto actualTrackIndex = sequencer_.findTrackIndexForInstance(instanceId);
        auto midiDevice = createLibreMidiIODevice(deviceState->apiName,
                                                  deviceName.empty() ? deviceState->label : deviceName,
                                                  "UAPMD Project",
                                                  "0.1");

        auto device = std::make_shared<UapmdMidiDevice>(midiDevice,
                                                         sequencer_.engine(),
                                                         instanceId,
                                                         actualTrackIndex,
                                                         deviceState->apiName,
                                                         deviceName.empty() ? deviceState->label : deviceName,
                                                         "UAPMD Project",
                                                         "0.1");

        if (auto group = sequencer_.engine()->groupForInstance(instanceId); group.has_value()) {
            device->group(group.value());
        }

        sequencer_.engine()->assignMidiDeviceToPlugin(instanceId, midiDevice);
        device->initialize();

        deviceState->device = device;
        if (!deviceName.empty()) {
            deviceState->label = deviceName;
        }
    }

    // Start the device and update state directly
    auto statusCode = deviceState->device->start();

    // Update DeviceState directly (no need for callback to do this)
    deviceState->running = (statusCode == 0);
    deviceState->hasError = (statusCode != 0);
    deviceState->statusMessage = (statusCode == 0) ? "Running" : std::format("Error (status {})", statusCode);

    // Populate result for callback notification
    result.success = (statusCode == 0);
    result.running = deviceState->running;
    result.statusMessage = deviceState->statusMessage;

    if (statusCode == 0) {
        std::cout << "Enabled UMP device for instance: " << instanceId << std::endl;
    } else {
        result.error = std::format("Failed to start device (status: {})", statusCode);
        std::cout << "Failed to enable UMP device for instance: " << instanceId
                  << " (status: " << statusCode << ")" << std::endl;
    }

    // Notify all registered callbacks (just for UI refresh)
    for (auto& cb : deviceEnabled) {
        cb(result);
    }
}

void uapmd::AppModel::disableUmpDevice(int32_t instanceId) {
    DeviceStateResult result;
    result.instanceId = instanceId;

    // Find the device for this instance
    std::shared_ptr<DeviceState> deviceState;
    {
        std::lock_guard lock(devicesMutex_);
        for (auto& entry : devices_) {
            auto state = entry.state;
            if (state && state->pluginInstances.count(instanceId) > 0) {
                deviceState = state;
                break;
            }
        }
    }

    if (!deviceState || !deviceState->device) {
        result.success = false;
        result.error = "Device not found for instance";
        result.statusMessage = "Error";
        for (auto& cb : deviceDisabled) {
            cb(result);
        }
        return;
    }

    // Clear MIDI device from plugin node to release the shared_ptr
    sequencer_.engine()->clearMidiDeviceFromPlugin(instanceId);

    // Stop and destroy the device to unregister the virtual MIDI port
    std::lock_guard guard(deviceState->mutex);
    if (deviceState->device) {
        deviceState->device->stop();
        // Destroy the device object to unregister the virtual MIDI port
        deviceState->device.reset();
    }

    // Update DeviceState directly (no need for callback to do this)
    deviceState->running = false;
    deviceState->hasError = false;
    deviceState->statusMessage = "Stopped";

    // Populate result for callback notification
    result.success = true;
    result.running = false;
    result.statusMessage = deviceState->statusMessage;

    std::cout << "Disabled UMP device for instance: " << instanceId << std::endl;

    // Notify all registered callbacks (just for UI refresh)
    for (auto& cb : deviceDisabled) {
        cb(result);
    }
}

void uapmd::AppModel::requestShowPluginUI(int32_t instanceId) {
    // Trigger callbacks - MainWindow will handle preparing window and calling showPluginUI()
    for (auto& cb : uiShowRequested) {
        cb(instanceId);
    }
}

void uapmd::AppModel::showPluginUI(int32_t instanceId, bool needsCreate, bool isFloating, void* parentHandle, std::function<bool(uint32_t, uint32_t)> resizeHandler) {
    UIStateResult result;
    result.instanceId = instanceId;

    auto* instance = sequencer_.engine()->getPluginInstance(instanceId);
    if (!instance) {
        result.success = false;
        result.error = "Plugin instance not found";
        for (auto& cb : uiShown) {
            cb(result);
        }
        return;
    }

    if (!instance->hasUISupport()) {
        result.success = false;
        result.error = "Plugin does not support UI";
        for (auto& cb : uiShown) {
            cb(result);
        }
        return;
    }

    // Create the UI if needed (first time showing)
    if (needsCreate) {
        if (!instance->createUI(isFloating, parentHandle, resizeHandler)) {
            result.success = false;
            result.error = "Failed to create plugin UI";
            for (auto& cb : uiShown) {
                cb(result);
            }
            return;
        }
        result.wasCreated = true;
    }

    // Show the UI
    if (!instance->showUI()) {
        result.success = false;
        result.error = "Failed to show plugin UI";
        for (auto& cb : uiShown) {
            cb(result);
        }
        return;
    }

    result.success = true;
    result.visible = true;

    // Notify all registered callbacks
    for (auto& cb : uiShown) {
        cb(result);
    }
}

void uapmd::AppModel::hidePluginUI(int32_t instanceId) {
    UIStateResult result;
    result.instanceId = instanceId;

    auto* instance = sequencer_.engine()->getPluginInstance(instanceId);
    if (!instance) {
        result.success = false;
        result.error = "Plugin instance not found";
        for (auto& cb : uiHidden) {
            cb(result);
        }
        return;
    }

    // Hide the UI
    if (instance->hasUISupport() && instance->isUIVisible()) {
        instance->hideUI();
    }

    result.success = true;
    result.visible = false;

    // Notify all registered callbacks
    for (auto& cb : uiHidden) {
        cb(result);
    }
}

void uapmd::TransportController::play() {
    sequencer_->engine()->startPlayback();
    isPlaying_ = true;
    isPaused_ = false;
}

void uapmd::TransportController::stop() {
    sequencer_->engine()->stopPlayback();
    isPlaying_ = false;
    isPaused_ = false;
    playbackPosition_ = 0.0f;
}

void uapmd::TransportController::pause() {
    sequencer_->engine()->pausePlayback();
    isPaused_ = true;
}

void uapmd::TransportController::resume() {
    sequencer_->engine()->resumePlayback();
    isPaused_ = false;
}

void uapmd::TransportController::record() {
    isRecording_ = !isRecording_;

    if (isRecording_)
        std::cout << "Starting recording" << std::endl;
    else
        std::cout << "Stopping recording" << std::endl;
}

void uapmd::TransportController::loadFile() {
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

    std::string filepath = selection.result()[0];

    auto reader = uapmd::createAudioFileReaderFromPath(filepath);
    if (!reader) {
        pfd::message("Load Failed",
                    "Could not load audio file: " + filepath + "\nSupported formats: WAV, FLAC, OGG",
                    pfd::choice::ok,
                    pfd::icon::error);
        return;
    }

    sequencer_->engine()->loadAudioFile(std::move(reader));

    currentFile_ = filepath;
    playbackLength_ = static_cast<float>(sequencer_->engine()->audioFileDurationSeconds());
    playbackPosition_ = 0.0f;

    std::cout << "File loaded: " << currentFile_ << std::endl;
}

void uapmd::TransportController::unloadFile() {
    // Stop playback if currently playing
    if (isPlaying_)
        stop();

    // Unload the audio file from the sequencer
    sequencer_->engine()->unloadAudioFile();

    // Clear state
    currentFile_.clear();
    playbackLength_ = 0.0f;
    playbackPosition_ = 0.0f;

    std::cout << "Audio file unloaded" << std::endl;
}

std::vector<uapmd::AppModel::DeviceEntry> uapmd::AppModel::getDevices() const {
    std::lock_guard lock(devicesMutex_);
    return devices_;  // Return copy
}

std::optional<std::shared_ptr<uapmd::AppModel::DeviceState>> uapmd::AppModel::getDeviceForInstance(int32_t instanceId) const {
    std::lock_guard lock(devicesMutex_);
    for (const auto& entry : devices_) {
        auto state = entry.state;
        if (state && state->pluginInstances.count(instanceId) > 0) {
            return state;
        }
    }
    return std::nullopt;
}

void uapmd::AppModel::updateDeviceLabel(int32_t instanceId, const std::string& label) {
    std::lock_guard lock(devicesMutex_);
    for (auto& entry : devices_) {
        auto state = entry.state;
        if (state) {
            std::lock_guard guard(state->mutex);
            if (state->pluginInstances.count(instanceId) > 0) {
                state->label = label;
                break;
            }
        }
    }
}

uapmd::AppModel::PluginStateResult uapmd::AppModel::loadPluginState(int32_t instanceId, const std::string& filepath) {
    PluginStateResult result;
    result.instanceId = instanceId;
    result.filepath = filepath;

    // Get plugin instance
    auto* instance = sequencer_.engine()->getPluginInstance(instanceId);
    if (!instance) {
        result.success = false;
        result.error = "Failed to get plugin instance";
        std::cerr << result.error << std::endl;
        return result;
    }

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
        result.success = false;
        result.error = std::format("Failed to load plugin state: {}", ex.what());
        std::cerr << result.error << std::endl;
        return result;
    }

    // Set plugin state
    instance->loadState(stateData);
    // Note: loadState doesn't return a status, so we assume success

    result.success = true;
    std::cout << "Plugin state loaded from: " << filepath << std::endl;
    return result;
}

uapmd::AppModel::PluginStateResult uapmd::AppModel::savePluginState(int32_t instanceId, const std::string& filepath) {
    PluginStateResult result;
    result.instanceId = instanceId;
    result.filepath = filepath;

    // Get plugin instance
    auto* instance = sequencer_.engine()->getPluginInstance(instanceId);
    if (!instance) {
        result.success = false;
        result.error = "Failed to get plugin instance";
        std::cerr << result.error << std::endl;
        return result;
    }

    // Get plugin state
    auto stateData = instance->saveState();
    if (stateData.empty()) {
        result.success = false;
        result.error = "Failed to retrieve plugin state";
        std::cerr << result.error << std::endl;
        return result;
    }

    // Save to file as binary blob
    try {
        std::ofstream file(filepath, std::ios::binary);
        if (!file.is_open()) {
            throw std::runtime_error("Failed to open file for writing");
        }
        file.write(reinterpret_cast<const char*>(stateData.data()), stateData.size());
        file.close();

        result.success = true;
        std::cout << "Plugin state saved to: " << filepath << std::endl;
    } catch (const std::exception& ex) {
        result.success = false;
        result.error = std::format("Failed to save plugin state: {}", ex.what());
        std::cerr << result.error << std::endl;
    }

    return result;
}
