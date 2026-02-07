
#include "uapmd/uapmd.hpp"
#include "AppModel.hpp"
#include <uapmd-data/priv/project/Smf2ClipReader.hpp>
#include <uapmd-data/priv/timeline/MidiClipSourceNode.hpp>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <exception>
#include <algorithm>

#define DEFAULT_AUDIO_BUFFER_SIZE 1024
#define DEFAULT_UMP_BUFFER_SIZE 65536
#define DEFAULT_SAMPLE_RATE 48000
#define FIXED_CHANNEL_COUNT 2

std::unique_ptr<uapmd::AppModel> model{};

uapmd::TransportController::TransportController(AppModel* appModel, RealtimeSequencer* sequencer)
    : appModel_(appModel), sequencer_(sequencer) {
}

std::shared_ptr<uapmd::MidiIOFeature> uapmd::AppModel::createMidiIOFeature(
    std::string apiName, std::string deviceName, std::string manufacturer, std::string version) {
    return createLibreMidiIODevice(apiName, deviceName, manufacturer, version);
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
        transportController_(std::make_unique<TransportController>(this, &sequencer_)),
        sample_rate_(sampleRate) {
    sequencer_.engine()->functionBlockManager()->setMidiIOManager(this);

    // Initialize timeline state
    timeline_.tempo = 120.0;
    timeline_.timeSignatureNumerator = 4;
    timeline_.timeSignatureDenominator = 4;
    timeline_.isPlaying = false;
    timeline_.loopEnabled = false;

    // Initialize timeline tracks (independent of SequencerTracks)
    auto& uapmdTracks = sequencer_.engine()->tracks();
    for (size_t i = 0; i < uapmdTracks.size(); ++i) {
        timeline_tracks_.push_back(std::make_unique<uapmd::TimelineTrack>(FIXED_CHANNEL_COUNT, static_cast<double>(sampleRate)));
    }

    // Start with a few empty tracks for the DAW layout
    constexpr int kInitialTrackCount = 3;
    for (int i = 0; i < kInitialTrackCount; ++i) {
        addTrack();
    }

    // Register audio preprocessing callback for timeline-based source processing
    sequencer_.engine()->setAudioPreprocessCallback(
        [this](uapmd::AudioProcessContext& process) {
            this->processAppTracksAudio(process);
        }
    );
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
            sequencer_.engine()->pluginHost()->performPluginScanning(false); // Load from cache, don't rescan

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
    for (auto plugins = sequencer_.engine()->pluginHost()->pluginCatalogEntries(); auto& plugin : plugins) {
        if (plugin.format() == format && plugin.pluginId() == pluginId) {
            pluginName = plugin.displayName();
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

    auto instantiateCallback = [this, config, deviceLabel, pluginName, format, pluginId](int32_t instanceId, int32_t trackIndex, std::string error) {
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

        {
            std::lock_guard lock(devicesMutex_);
            devices_.push_back(DeviceEntry{nextDeviceId_++, state});
        }

        // Reuse the dedicated logic for MIDI device initialization when supported.
        if (midiApiSupportsUmp(config.apiName)) {
            enableUmpDevice(instanceId, deviceLabel);
        } else {
            state->running = false;
            state->hasError = true;
            state->statusMessage = "Virtual MIDI 2.0 devices are unavailable on this platform.";
        }

        result.device = state->device;

        // Notify all registered callbacks
        for (auto& cb : instanceCreated) {
            cb(result);
        }
    };

    if (trackIndex < 0) {
        trackIndex = addTrack();
        if (trackIndex < 0) {
            instantiateCallback(-1, -1, "Failed to add track for new plugin instance");
            return;
        }
    } else {
        auto& tracks = sequencer_.engine()->tracks();
        if (trackIndex >= static_cast<int32_t>(tracks.size())) {
            instantiateCallback(-1, -1, std::format("Invalid track index {}", trackIndex));
            return;
        }
    }

    sequencer_.engine()->addPluginToTrack(trackIndex, formatCopy, pluginIdCopy, instantiateCallback);
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

    disableUmpDevice(instanceId);

    // Stop and remove virtual MIDI device if it exists
    {
        std::lock_guard lock(devicesMutex_);
        for (auto it = devices_.begin(); it != devices_.end(); ++it) {
            auto state = it->state;
            if (state) {
                std::lock_guard guard(state->mutex);
                if (state->pluginInstances.count(instanceId) > 0) {
                    devices_.erase(it);
                    break;
                }
            }
        }
    }

    sequencer_.engine()->removePluginInstance(instanceId);
    sequencer().engine()->functionBlockManager()->deleteEmptyDevices();

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
        for (auto& cb : enableDeviceCompleted) {
            cb(result);
        }
        return;
    }

    // Lock the device state for modifications
    std::lock_guard guard(deviceState->mutex);

    if (!midiApiSupportsUmp(deviceState->apiName)) {
        deviceState->running = false;
        deviceState->hasError = true;
        deviceState->statusMessage = "Virtual MIDI 2.0 devices are unavailable on this platform.";
        result.success = false;
        result.error = deviceState->statusMessage;
        result.statusMessage = deviceState->statusMessage;
        for (auto& cb : enableDeviceCompleted) {
            cb(result);
        }
        return;
    }

    // If device was destroyed (disabled), recreate it
    if (!deviceState->device) {
        auto fbManager = sequencer_.engine()->functionBlockManager();

        auto fbDeviceIndex = fbManager->create();
        auto fbDevice = fbManager->getFunctionDeviceByIndex(fbDeviceIndex);
        const auto track = sequencer_.engine()->tracks()[sequencer_.engine()->findTrackIndexForInstance(instanceId)];
        const auto pluginNode = track ? track->graph().getPluginNode(instanceId) : nullptr;
        if (!fbDevice->createFunctionBlock(deviceState->apiName, pluginNode, instanceId,
                                               deviceName.empty() ? deviceState->label : deviceName,
                                               "UAPMD Project",
                                               "0.1")) {
            deviceState->running = false;
            deviceState->hasError = true;
            deviceState->statusMessage = "Failed to create virtual MIDI device";
            result.success = false;
            result.error = deviceState->statusMessage;
            result.statusMessage = deviceState->statusMessage;
            for (auto& cb : enableDeviceCompleted) {
                cb(result);
            }
            return;
        }
        auto fb = fbManager->getFunctionDeviceByInstanceId(instanceId);

        sequencer_.engine()->getPluginInstance(instanceId)->assignMidiDeviceToPlugin(fb->midiIO());
        fb->initialize();

        deviceState->device = fb;
        if (!deviceName.empty()) {
            deviceState->label = deviceName;
        }
    }

    // Update DeviceState directly (no need for callback to do this)
    deviceState->running = true;
    deviceState->hasError = false;
    deviceState->statusMessage = "Running";

    // Populate result for callback notification
    result.success = true;
    result.running = deviceState->running;
    result.statusMessage = deviceState->statusMessage;

    std::cout << "Enabled UMP device for instance: " << instanceId << std::endl;

    // Notify all registered callbacks (just for UI refresh)
    for (auto& cb : enableDeviceCompleted) {
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
        for (auto& cb : disableDeviceCompleted) {
            cb(result);
        }
        return;
    }

    // Clear MIDI device from plugin node to release the shared_ptr
    sequencer_.engine()->getPluginInstance(instanceId)->clearMidiDeviceFromPlugin();
    if (auto fb = sequencer().engine()->functionBlockManager()->getFunctionDeviceForInstance(instanceId))
        fb->destroyDevice(instanceId);

    // Stop and destroy the device to unregister the virtual MIDI port
    std::lock_guard guard(deviceState->mutex);
    if (deviceState->device) {
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
    for (auto& cb : disableDeviceCompleted) {
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
    appModel_->timeline().isPlaying = true;
    isPlaying_ = true;
    isPaused_ = false;
}

void uapmd::TransportController::stop() {
    sequencer_->engine()->stopPlayback();
    appModel_->timeline().isPlaying = false;
    appModel_->timeline().playheadPosition.samples = 0;
    appModel_->timeline().playheadPosition.legacy_beats = 0.0;
    isPlaying_ = false;
    isPaused_ = false;
}

void uapmd::TransportController::pause() {
    sequencer_->engine()->pausePlayback();
    appModel_->timeline().isPlaying = false;
    isPaused_ = true;
}

void uapmd::TransportController::resume() {
    sequencer_->engine()->resumePlayback();
    appModel_->timeline().isPlaying = true;
    isPaused_ = false;
}

void uapmd::TransportController::record() {
    isRecording_ = !isRecording_;

    if (isRecording_)
        std::cout << "Starting recording" << std::endl;
    else
        std::cout << "Stopping recording" << std::endl;
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

// Timeline and clip management

uapmd::AppModel::ClipAddResult uapmd::AppModel::addClipToTrack(
    int32_t trackIndex,
    const uapmd::TimelinePosition& position,
    std::unique_ptr<uapmd::AudioFileReader> reader,
    const std::string& filepath
) {
    ClipAddResult result;

    if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(timeline_tracks_.size())) {
        result.error = "Invalid track index";
        return result;
    }

    // Detect MIDI files by extension and route to addMidiClipToTrack
    if (!filepath.empty()) {
        std::filesystem::path path(filepath);
        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        if (ext == ".mid" || ext == ".midi" || ext == ".smf") {
            // MIDI file - route to MIDI clip handler
            return addMidiClipToTrack(trackIndex, position, filepath);
        }
    }

    if (!reader) {
        result.error = "Invalid audio file reader";
        return result;
    }

    try {
        // Create source node for this audio file
        int32_t sourceNodeId = next_source_node_id_++;
        auto sourceNode = std::make_unique<uapmd::AudioFileSourceNode>(
            sourceNodeId,
            std::move(reader),
            static_cast<double>(sample_rate_)
        );

        // Get the duration of the audio file
        int64_t durationSamples = sourceNode->totalLength();

        // Create clip data
        uapmd::ClipData clip;
        clip.position = position;
        clip.durationSamples = durationSamples;
        clip.sourceNodeInstanceId = sourceNodeId;
        clip.gain = 1.0;
        clip.muted = false;
        clip.filepath = filepath;

        // Add clip to track
        int32_t clipId = timeline_tracks_[trackIndex]->addClip(clip, std::move(sourceNode));

        if (clipId >= 0) {
            result.success = true;
            result.clipId = clipId;
            result.sourceNodeId = sourceNodeId;
        } else {
            result.error = "Failed to add clip to track";
        }

    } catch (const std::exception& ex) {
        result.error = std::format("Exception adding clip: {}", ex.what());
    }

    return result;
}

uapmd::AppModel::ClipAddResult uapmd::AppModel::addMidiClipToTrack(
    int32_t trackIndex,
    const uapmd::TimelinePosition& position,
    const std::string& filepath
) {
    ClipAddResult result;

    if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(timeline_tracks_.size())) {
        result.error = "Invalid track index";
        return result;
    }

    try {
        // Convert SMF to UMP using Smf2ClipReader
        auto clipInfo = uapmd::Smf2ClipReader::readAnyFormat(filepath);
        if (!clipInfo.success) {
            result.error = clipInfo.error;
            return result;
        }

        // Create MIDI source node
        int32_t sourceNodeId = next_source_node_id_++;
        auto sourceNode = std::make_unique<uapmd::MidiClipSourceNode>(
            sourceNodeId,
            std::move(clipInfo.ump_data),
            std::move(clipInfo.ump_tick_timestamps),
            clipInfo.tick_resolution,
            clipInfo.tempo,
            static_cast<double>(sample_rate_)
        );

        // Get duration from source node
        int64_t durationSamples = sourceNode->totalLength();

        // Create MIDI clip data
        uapmd::ClipData clip;
        clip.clipType = uapmd::ClipType::Midi;
        clip.position = position;
        clip.durationSamples = durationSamples;
        clip.sourceNodeInstanceId = sourceNodeId;
        clip.filepath = filepath;
        clip.tickResolution = clipInfo.tick_resolution;
        clip.clipTempo = clipInfo.tempo;
        clip.gain = 1.0;
        clip.muted = false;
        clip.name = std::filesystem::path(filepath).stem().string();

        // Add clip to track
        int32_t clipId = timeline_tracks_[trackIndex]->addClip(clip, std::move(sourceNode));

        if (clipId >= 0) {
            result.success = true;
            result.clipId = clipId;
            result.sourceNodeId = sourceNodeId;
        } else {
            result.error = "Failed to add MIDI clip to track";
        }

    } catch (const std::exception& ex) {
        result.error = std::format("Failed to load MIDI file: {}", ex.what());
    }

    return result;
}

bool uapmd::AppModel::removeClipFromTrack(int32_t trackIndex, int32_t clipId) {
    if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(timeline_tracks_.size()))
        return false;

    return timeline_tracks_[trackIndex]->removeClip(clipId);
}

int32_t uapmd::AppModel::addDeviceInputToTrack(
    int32_t trackIndex,
    const std::vector<uint32_t>& channelIndices
) {
    if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(timeline_tracks_.size()))
        return -1;

    int32_t sourceNodeId = next_source_node_id_++;
    uint32_t channelCount = channelIndices.empty() ? 2 : static_cast<uint32_t>(channelIndices.size());

    auto sourceNode = std::make_unique<uapmd::DeviceInputSourceNode>(
        sourceNodeId,
        channelCount,
        channelIndices
    );

    if (timeline_tracks_[trackIndex]->addDeviceInputSource(std::move(sourceNode)))
        return sourceNodeId;

    return -1;
}

std::vector<uapmd::TimelineTrack*> uapmd::AppModel::getTimelineTracks() {
    std::vector<uapmd::TimelineTrack*> tracks;
    tracks.reserve(timeline_tracks_.size());
    for (auto& track : timeline_tracks_) {
        tracks.push_back(track.get());
    }
    return tracks;
}

void uapmd::AppModel::notifyTrackLayoutChanged(const TrackLayoutChange& change) {
    for (auto& cb : trackLayoutChanged) {
        cb(change);
    }
}

int32_t uapmd::AppModel::addTrack() {
    if (!hidden_tracks_.empty()) {
        auto it = hidden_tracks_.begin();
        int32_t reusedIndex = *it;
        hidden_tracks_.erase(it);
        notifyTrackLayoutChanged(TrackLayoutChange{TrackLayoutChange::Type::Added, reusedIndex});
        return reusedIndex;
    }

    auto trackIndex = sequencer_.engine()->addEmptyTrack();
    if (trackIndex < 0)
        return -1;

    while (timeline_tracks_.size() <= static_cast<size_t>(trackIndex)) {
        timeline_tracks_.push_back(
            std::make_unique<uapmd::TimelineTrack>(FIXED_CHANNEL_COUNT, static_cast<double>(sample_rate_))
        );
    }
    notifyTrackLayoutChanged(TrackLayoutChange{TrackLayoutChange::Type::Added, trackIndex});
    return trackIndex;
}

bool uapmd::AppModel::removeTrack(int32_t trackIndex) {
    auto& uapmdTracks = sequencer_.engine()->tracks();
    if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(uapmdTracks.size()))
        return false;

    auto instances = uapmdTracks[trackIndex]->orderedInstanceIds();
    for (int32_t instanceId : instances) {
        removePluginInstance(instanceId);
    }

    if (trackIndex >= 0 && trackIndex < static_cast<int32_t>(timeline_tracks_.size())) {
        timeline_tracks_[trackIndex]->clipManager().clearAll();
    }

    hidden_tracks_.insert(trackIndex);
    notifyTrackLayoutChanged(TrackLayoutChange{TrackLayoutChange::Type::Removed, trackIndex});
    return true;
}

void uapmd::AppModel::removeAllTracks() {
    auto trackCount = static_cast<int32_t>(sequencer_.engine()->tracks().size());
    for (int32_t i = 0; i < trackCount; ++i) {
        removeTrack(i);
    }
    notifyTrackLayoutChanged(TrackLayoutChange{TrackLayoutChange::Type::Cleared, -1});
}

// Audio processing callback - processes app tracks with timeline
void uapmd::AppModel::processAppTracksAudio(AudioProcessContext& process) {
    // Update timeline state if playing
    if (timeline_.isPlaying) {
        timeline_.playheadPosition.samples += process.frameCount();

        // Handle loop region
        if (timeline_.loopEnabled) {
            if (timeline_.playheadPosition.samples >= timeline_.loopEnd.samples) {
                timeline_.playheadPosition.samples = timeline_.loopStart.samples;
            }
        }

        // Update legacy_beats (keep for backwards compatibility with serialization)
        double secondsPerBeat = 60.0 / timeline_.tempo;
        int64_t samplesPerBeat = static_cast<int64_t>(secondsPerBeat * sample_rate_);
        if (samplesPerBeat > 0) {
            timeline_.playheadPosition.legacy_beats =
                static_cast<double>(timeline_.playheadPosition.samples) / samplesPerBeat;
        }

        // Sync timeline state to MasterContext
        auto& masterCtx = process.trackContext()->masterContext();
        masterCtx.playbackPositionSamples(timeline_.playheadPosition.samples);
        masterCtx.isPlaying(timeline_.isPlaying);
        masterCtx.tempo(static_cast<uint16_t>(timeline_.tempo));
        masterCtx.timeSignatureNumerator(timeline_.timeSignatureNumerator);
        masterCtx.timeSignatureDenominator(timeline_.timeSignatureDenominator);
    }

    // Get the sequence process context from engine
    auto& sequenceData = sequencer_.engine()->data();

    // Process each timeline track
    for (size_t i = 0; i < timeline_tracks_.size() && i < sequenceData.tracks.size(); ++i) {
        auto* trackContext = sequenceData.tracks[i];

        // Copy device inputs from main process context to track context
        if (process.audioInBusCount() > 0 && trackContext->audioInBusCount() > 0) {
            const uint32_t deviceChannels = std::min(
                static_cast<uint32_t>(process.inputChannelCount(0)),
                static_cast<uint32_t>(trackContext->inputChannelCount(0))
            );

            for (uint32_t ch = 0; ch < deviceChannels; ++ch) {
                const float* src = process.getFloatInBuffer(0, ch);
                float* dst = trackContext->getFloatInBuffer(0, ch);
                if (src && dst) {
                    std::memcpy(dst, src, process.frameCount() * sizeof(float));
                }
            }
        }

        // Process timeline track
        // Timeline writes to trackContext->audio_in[0]
        // Then sequencer track's plugins process input->output
        // SequencerEngine will mix track outputs into main output
        timeline_tracks_[i]->processAudio(*trackContext, timeline_);
    }
}
