
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <iostream>
#include <optional>
#include <utility>
#include <cmidi2.h>
#include "uapmd/uapmd.hpp"
#include "uapmd/priv/sequencer/AudioPluginSequencer.hpp"
#include "../../../include/uapmd/priv/plugin-api/AudioPluginHostingAPI.hpp"
#include "../Midi/UapmdNodeUmpMapper.hpp"

// Note: audio file decoding is abstracted behind uapmd::AudioFileReader interface now.

void uapmd::AudioPluginSequencer::configureTrackRouting(AudioPluginTrack* track) {
    if (!track)
        return;
    track->setGroupResolver([this](int32_t instanceId) {
        auto group = groupForInstanceOptional(instanceId);
        return group.has_value() ? group.value() : static_cast<uint8_t>(0xFF);
    });
    track->setEventOutputCallback([this](int32_t instanceId, const uapmd_ump_t* data, size_t bytes) {
        dispatchPluginOutput(instanceId, data, bytes);
    });
}

uint8_t uapmd::AudioPluginSequencer::assignGroup(int32_t instanceId) {
    auto it = plugin_groups_.find(instanceId);
    if (it != plugin_groups_.end())
        return it->second;

    uint8_t group = 0xFF;
    if (!free_groups_.empty()) {
        group = free_groups_.back();
        free_groups_.pop_back();
    } else {
        if (next_group_ <= 0x0F) {
            group = next_group_;
            ++next_group_;
        } else {
            remidy::Logger::global()->logError("No available UMP groups for plugin instance {}", instanceId);
            group = 0xFF;
        }
    }
    if (group != 0xFF) {
        plugin_groups_[instanceId] = group;
        group_to_instance_[group] = instanceId;
    }
    return group;
}

void uapmd::AudioPluginSequencer::releaseGroup(int32_t instanceId) {
    auto it = plugin_groups_.find(instanceId);
    if (it == plugin_groups_.end())
        return;
    auto group = it->second;
    plugin_groups_.erase(it);
    if (group != 0xFF) {
        group_to_instance_.erase(group);
        if (group <= 0x0F)
            free_groups_.push_back(group);
    }
}

std::optional<uint8_t> uapmd::AudioPluginSequencer::groupForInstanceOptional(int32_t instanceId) const {
    auto it = plugin_groups_.find(instanceId);
    if (it == plugin_groups_.end())
        return std::nullopt;
    return it->second;
}

std::optional<int32_t> uapmd::AudioPluginSequencer::instanceForGroupOptional(uint8_t group) const {
    auto it = group_to_instance_.find(group);
    if (it == group_to_instance_.end())
        return std::nullopt;
    return it->second;
}

void uapmd::AudioPluginSequencer::registerParameterListener(int32_t instanceId, AudioPluginInstanceAPI* node) {
    if (!node)
        return;
    auto token = node->addParameterChangeListener(
        [this, instanceId](uint32_t paramIndex, double plainValue) {
            std::lock_guard<std::mutex> lock(pending_parameter_mutex_);
            pending_parameter_updates_[instanceId].push_back({static_cast<int32_t>(paramIndex), plainValue});
        });
    if (token == 0)
        return;
    std::lock_guard<std::mutex> lock(parameter_listener_mutex_);
    parameter_listener_tokens_[instanceId] = token;
}

void uapmd::AudioPluginSequencer::unregisterParameterListener(int32_t instanceId) {
    remidy::PluginParameterSupport::ParameterChangeListenerId token{0};
    {
        std::lock_guard<std::mutex> lock(parameter_listener_mutex_);
        auto it = parameter_listener_tokens_.find(instanceId);
        if (it == parameter_listener_tokens_.end())
            return;
        token = it->second;
        parameter_listener_tokens_.erase(it);
    }
    if (token == 0)
        return;
    auto* node = getPluginInstance(instanceId);
    if (node)
        node->removeParameterChangeListener(token);
}

void uapmd::AudioPluginSequencer::dispatchPluginOutput(int32_t instanceId, const uapmd_ump_t* data, size_t bytes) {
    if (!data || bytes == 0)
        return;

    auto groupOpt = groupForInstanceOptional(instanceId);
    if (!groupOpt.has_value())
        return;
    auto group = groupOpt.value();

    auto handlers = std::atomic_load_explicit(&plugin_output_handlers_, std::memory_order_acquire);
    if (!handlers)
        return;
    auto it = handlers->find(instanceId);
    if (it == handlers->end() || !it->second)
        return;

    if (bytes > plugin_output_scratch_.size() * sizeof(uapmd_ump_t))
        return;

    auto* scratch = plugin_output_scratch_.data();
    std::memcpy(scratch, data, bytes);

    // Process UMP messages and extract parameter changes
    size_t offset = 0;
    auto* byteView = reinterpret_cast<uint8_t*>(scratch);
    while (offset < bytes) {
        auto* msg = reinterpret_cast<cmidi2_ump*>(byteView + offset);
        auto size = cmidi2_ump_get_message_size_bytes(msg);
        auto* words = reinterpret_cast<uint32_t*>(byteView + offset);

        // Check for NRPN messages (parameter changes)
        uint8_t messageType = (words[0] >> 28) & 0xF;
        if (messageType == 4) { // MIDI 2.0 Channel Voice Message (64-bit)
            uint8_t status = (words[0] >> 16) & 0xF0;
            if (status == 0x30) { // NRPN
                uint8_t bank = (words[0] >> 8) & 0x7F;
                uint8_t index = words[0] & 0x7F;
                uint32_t value32 = words[1];

                // Reconstruct parameter ID: bank * 128 + index
                int32_t paramId = (bank * 128) + index;
                double value = static_cast<double>(value32) / 4294967295.0;

                // Store parameter update
                {
                    std::lock_guard<std::mutex> parameterLock(pending_parameter_mutex_);
                    pending_parameter_updates_[instanceId].push_back({paramId, value});
                }
            }
        }

        words[0] = (words[0] & 0xF0FFFFFF) | (static_cast<uint32_t>(group) << 24);
        offset += size;
    }

    it->second(reinterpret_cast<uapmd_ump_t*>(scratch), bytes);
}

void uapmd::AudioPluginSequencer::refreshFunctionBlockMappings() {
    plugin_function_blocks_.clear();
    auto& tracks = sequencer->tracks();
    for (size_t trackIndex = 0; trackIndex < tracks.size(); ++trackIndex) {
        auto* track = tracks[trackIndex];
        if (!track)
            continue;
        configureTrackRouting(track);
        auto plugins = track->graph().plugins();
        for (auto* plugin : plugins) {
            if (!plugin)
                continue;
            auto instanceId = plugin->instanceId();
            plugin_function_blocks_[instanceId] = FunctionBlockRoute{track, static_cast<int32_t>(trackIndex)};
            assignGroup(instanceId);
        }
    }
}

std::optional<uapmd::AudioPluginSequencer::RouteResolution> uapmd::AudioPluginSequencer::resolveTarget(int32_t trackOrInstanceId) {
    auto instanceIt = plugin_function_blocks_.find(trackOrInstanceId);
    if (instanceIt != plugin_function_blocks_.end()) {
        return RouteResolution{instanceIt->second.track, instanceIt->second.trackIndex, trackOrInstanceId};
    }

    if (auto instFromGroup = instanceForGroupOptional(static_cast<uint8_t>(trackOrInstanceId)); instFromGroup.has_value()) {
        auto mapping = plugin_function_blocks_.find(instFromGroup.value());
        if (mapping != plugin_function_blocks_.end()) {
            return RouteResolution{mapping->second.track, mapping->second.trackIndex, instFromGroup.value()};
        }
    }

    if (trackOrInstanceId < 0)
        return std::nullopt;

    auto& tracks = sequencer->tracks();
    if (static_cast<size_t>(trackOrInstanceId) >= tracks.size())
        return std::nullopt;

    auto* track = tracks[static_cast<size_t>(trackOrInstanceId)];
    if (!track)
        return std::nullopt;

    configureTrackRouting(track);
    auto plugins = track->graph().plugins();
    if (plugins.empty())
        return std::nullopt;

    auto* first = plugins.front();
    if (!first)
        return std::nullopt;

    auto instanceId = first->instanceId();
    plugin_function_blocks_[instanceId] = FunctionBlockRoute{track, static_cast<int32_t>(trackOrInstanceId)};
    assignGroup(instanceId);
    return RouteResolution{track, static_cast<int32_t>(trackOrInstanceId), instanceId};
}


uapmd::AudioPluginSequencer::AudioPluginSequencer(
    size_t audioBufferSizeInFrames,
    size_t umpBufferSizeInBytes,
    int32_t sampleRate,
    DeviceIODispatcher* dispatcher
) : buffer_size_in_frames(audioBufferSizeInFrames),
    ump_buffer_size_in_bytes(umpBufferSizeInBytes), sample_rate(sampleRate),
    dispatcher(dispatcher),
    plugin_host_pal(AudioPluginHostingAPI::instance()),
    sequencer(SequencerEngine::create(sampleRate, buffer_size_in_frames, umpBufferSizeInBytes, plugin_host_pal)),
    plugin_output_handlers_(std::make_shared<HandlerMap>()),
    plugin_output_scratch_(umpBufferSizeInBytes / sizeof(uapmd_ump_t), 0) {
    // Configure default channels based on audio device
    auto audioDevice = dispatcher->audio();
    const auto inputChannels = audioDevice ? std::max(audioDevice->inputChannels(), 2u) : 2;
    const auto outputChannels = audioDevice ? audioDevice->outputChannels() : 2;
    sequencer->setDefaultChannels(inputChannels, outputChannels);

    auto manager = AudioIODeviceManager::instance();
    auto logger = remidy::Logger::global();
    AudioIODeviceManager::Configuration audioConfig{ .logger = logger };
    manager->initialize(audioConfig);

    // FIXME: enable MIDI devices
    dispatcher->configure(umpBufferSizeInBytes, manager->open());

    dispatcher->addCallback([&](uapmd::AudioProcessContext& process) {
        // Delegate all master audio processing to SequencerEngine
        return sequencer->processAudio(process);
    });
}

uapmd::AudioPluginSequencer::~AudioPluginSequencer() {
    stopAudio();
}

uapmd::PluginCatalog& uapmd::AudioPluginSequencer::catalog() {
    return plugin_host_pal->catalog();
}

void uapmd::AudioPluginSequencer::performPluginScanning(bool rescan) {
    plugin_host_pal->performPluginScanning(rescan);
}

bool uapmd::AudioPluginSequencer::offlineRendering() const {
    return offline_rendering_.load(std::memory_order_acquire);
}

void uapmd::AudioPluginSequencer::offlineRendering(bool enabled) {
    offline_rendering_.store(enabled, std::memory_order_release);
}

std::vector<uapmd::AudioPluginSequencer::ParameterUpdate> uapmd::AudioPluginSequencer::getParameterUpdates(int32_t instanceId) {
    std::lock_guard<std::mutex> lock(pending_parameter_mutex_);
    auto it = pending_parameter_updates_.find(instanceId);
    if (it == pending_parameter_updates_.end())
        return {};
    auto updates = std::move(it->second);
    pending_parameter_updates_.erase(it);
    return updates;
}

std::vector<int32_t> uapmd::AudioPluginSequencer::getInstanceIds() {
    std::vector<int32_t> instances;
    for (auto& track : sequencer->tracks()) {
        for (auto plugin : track->graph().plugins()) {
            instances.push_back(plugin->instanceId());
        }
    }
    return instances;
}

std::string uapmd::AudioPluginSequencer::getPluginName(int32_t instanceId) {
    auto* instance = getPluginInstance(instanceId);
    // Get plugin ID and look it up in the catalog
    std::string pluginId = instance->pluginId();
    std::string format = instance->formatName();

    // Search in the catalog for display name
    auto plugins = plugin_host_pal->catalog().getPlugins();
    for (auto* catalogPlugin : plugins) {
        if (catalogPlugin->pluginId() == pluginId && catalogPlugin->format() == format)
            return catalogPlugin->displayName();
    }
    return "Plugin " + std::to_string(instanceId);
}

std::string uapmd::AudioPluginSequencer::getPluginFormat(int32_t instanceId) {
    auto* instance = getPluginInstance(instanceId);
    return instance->formatName();
}

std::vector<uapmd::AudioPluginSequencer::TrackInfo> uapmd::AudioPluginSequencer::getTrackInfos() {
    std::vector<TrackInfo> info;
    auto catalogPlugins = plugin_host_pal->catalog().getPlugins();
    auto displayNameFor = [&](const std::string& format, const std::string& pluginId) -> std::string {
        for (auto* entry : catalogPlugins) {
            if (entry->format() == format && entry->pluginId() == pluginId) {
                return entry->displayName();
            }
        }
        return pluginId;
    };

    auto& tracksRef = sequencer->tracks();
    info.reserve(tracksRef.size());
    for (size_t i = 0; i < tracksRef.size(); ++i) {
        TrackInfo trackInfo;
        trackInfo.trackIndex = static_cast<int32_t>(i);
        for (auto* plugin : tracksRef[i]->graph().plugins()) {
            PluginNodeInfo nodeInfo;
            nodeInfo.instanceId = plugin->instanceId();
            nodeInfo.pluginId = plugin->pal()->pluginId();
            nodeInfo.format = plugin->pal()->formatName();
            nodeInfo.displayName = displayNameFor(nodeInfo.format, nodeInfo.pluginId);
            trackInfo.nodes.push_back(std::move(nodeInfo));
        }
        info.push_back(std::move(trackInfo));
    }
    return info;
}

int32_t uapmd::AudioPluginSequencer::findTrackIndexForInstance(int32_t instanceId) const {
    auto& tracksRef = sequencer->tracks();
    for (size_t i = 0; i < tracksRef.size(); ++i) {
        for (auto* plugin : tracksRef[i]->graph().plugins()) {
            if (plugin->instanceId() == instanceId) {
                return static_cast<int32_t>(i);
            }
        }
    }
    return -1;
}

uapmd::AudioPluginNode* uapmd::AudioPluginSequencer::findPluginNodeByInstance(int32_t instanceId) {
    auto& tracksRef = sequencer->tracks();
    for (auto* track : tracksRef) {
        if (!track)
            continue;
        for (auto* plugin : track->graph().plugins()) {
            if (plugin && plugin->instanceId() == instanceId)
                return plugin;
        }
    }
    return nullptr;
}

void uapmd::AudioPluginSequencer::addSimplePluginTrack(
    std::string& format,
    std::string& pluginId,
    std::function<void(int32_t instanceId, std::string error)> callback
) {
    sequencer->addSimpleTrack(format, pluginId,
        [this, callback](AudioPluginNode* node, AudioPluginTrack* track, int32_t trackIndex, std::string error) {
            if (!node || !track || !error.empty()) {
                if (callback)
                    callback(-1, error);
                return;
            }

            // Add live-specific features
            auto instanceId = node->instanceId();
            configureTrackRouting(track);
            plugin_function_blocks_[instanceId] = FunctionBlockRoute{track, trackIndex};
            assignGroup(instanceId);

            AudioPluginInstanceAPI* palPtr = node->pal();
            {
                std::lock_guard<std::mutex> lock(instance_map_mutex_);
                plugin_instances_[instanceId] = palPtr;
                plugin_bypassed_[instanceId] = false;
            }

            registerParameterListener(instanceId, palPtr);
            refreshFunctionBlockMappings();

            if (callback)
                callback(instanceId, "");
        }
    );
}

void uapmd::AudioPluginSequencer::addPluginToTrack(
    int32_t trackIndex,
    std::string& format,
    std::string& pluginId,
    std::function<void(int32_t instanceId, std::string error)> callback
) {
    sequencer->addPluginToTrack(trackIndex, format, pluginId,
        [this, trackIndex, cb = std::move(callback)](AudioPluginNode* node, std::string error) mutable {
            if (!node || !error.empty()) {
                if (cb)
                    cb(-1, error);
                return;
            }

            // Get track for live feature setup
            auto& tracksRef = sequencer->tracks();
            if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= tracksRef.size()) {
                if (cb)
                    cb(-1, std::format("Track {} no longer exists", trackIndex));
                return;
            }
            auto* track = tracksRef[static_cast<size_t>(trackIndex)];

            // Add live-specific features
            auto instanceId = node->instanceId();
            configureTrackRouting(track);
            plugin_function_blocks_[instanceId] = FunctionBlockRoute{track, trackIndex};
            assignGroup(instanceId);

            AudioPluginInstanceAPI* palPtr = node->pal();
            {
                std::lock_guard<std::mutex> lock(instance_map_mutex_);
                plugin_instances_[instanceId] = palPtr;
                plugin_bypassed_[instanceId] = false;
            }

            registerParameterListener(instanceId, palPtr);
            refreshFunctionBlockMappings();

            if (cb)
                cb(instanceId, "");
        }
    );
}

bool uapmd::AudioPluginSequencer::removePluginInstance(int32_t instanceId) {
    // Destroy UI before removing the instance
    auto* instance = getPluginInstance(instanceId);
    instance->destroyUI();
    unregisterParameterListener(instanceId);

    plugin_function_blocks_.erase(instanceId);
    releaseGroup(instanceId);
    setPluginOutputHandler(instanceId, nullptr);
    {
        std::lock_guard<std::mutex> lock(instance_map_mutex_);
        plugin_instances_.erase(instanceId);
        plugin_bypassed_.erase(instanceId);
    }
    const auto removed = sequencer->removePluginInstance(instanceId);
    if (removed) {
        refreshFunctionBlockMappings();
    }
    return removed;
}

uapmd::AudioPluginInstanceAPI* uapmd::AudioPluginSequencer::getPluginInstance(int32_t instanceId) {
    std::lock_guard<std::mutex> lock(instance_map_mutex_);
    auto it = plugin_instances_.find(instanceId);
    if (it != plugin_instances_.end())
        return it->second;
    return nullptr;
}

bool uapmd::AudioPluginSequencer::isPluginBypassed(int32_t instanceId) {
    std::lock_guard<std::mutex> lock(instance_map_mutex_);
    auto it = plugin_bypassed_.find(instanceId);
    if (it != plugin_bypassed_.end())
        return it->second;
    return false;
}

void uapmd::AudioPluginSequencer::setPluginBypassed(int32_t instanceId, bool bypassed) {
    std::lock_guard<std::mutex> lock(instance_map_mutex_);
    plugin_bypassed_[instanceId] = bypassed;
}

void addMessage64(cmidi2_ump* dst, int64_t ump) {
    cmidi2_ump_write64(dst, ump);
}

void uapmd::AudioPluginSequencer::sendNoteOn(int32_t targetId, int32_t note) {
    auto route = resolveTarget(targetId);
    if (!route || !route->track) {
        remidy::Logger::global()->logError(std::format("sendNoteOn unresolved target {}", targetId).c_str());
        return;
    }

    auto group = groupForInstanceOptional(route->instanceId).value_or(0);
    cmidi2_ump umps[2];
    auto ump = cmidi2_ump_midi2_note_on(group, 0, note, 0, 0xF800, 0);
    addMessage64(umps, ump);
    if (!route->track->scheduleEvents(0, umps, 8))
        remidy::Logger::global()->logError(std::format("Failed to enqueue note on event for target {}: {}", targetId, note).c_str());
}

void uapmd::AudioPluginSequencer::sendNoteOff(int32_t targetId, int32_t note) {
    auto route = resolveTarget(targetId);
    if (!route || !route->track) {
        remidy::Logger::global()->logError(std::format("sendNoteOff unresolved target {}", targetId).c_str());
        return;
    }
    auto group = groupForInstanceOptional(route->instanceId).value_or(0);
    cmidi2_ump umps[2];
    auto ump = cmidi2_ump_midi2_note_off(group, 0, note, 0, 0xF800, 0);
    addMessage64(umps, ump);
    if (!route->track->scheduleEvents(0, umps, 8))
        remidy::Logger::global()->logError(std::format("Failed to enqueue note off event for target {}: {}", targetId, note).c_str());
}

void uapmd::AudioPluginSequencer::setParameterValue(int32_t instanceId, int32_t index, double value) {
    auto* instance = getPluginInstance(instanceId);
    instance->setParameterValue(index, value);
    remidy::Logger::global()->logError(std::format("Native parameter change {}: {} = {}", instanceId, index, value).c_str());
}

void uapmd::AudioPluginSequencer::enqueueUmp(int32_t targetId, uapmd_ump_t *ump, size_t sizeInBytes, uapmd_timestamp_t timestamp) {
    auto route = resolveTarget(targetId);
    if (!route || !route->track) {
        remidy::Logger::global()->logError(std::format("Failed to enqueue UMP events: unresolved target {}", targetId).c_str());
        return;
    }
    if (route->instanceId >= 0) {
        if (auto group = groupForInstanceOptional(route->instanceId); group.has_value()) {
            auto* bytes = reinterpret_cast<uint8_t*>(ump);
            size_t offset = 0;
            while (offset < sizeInBytes) {
                auto* msg = reinterpret_cast<cmidi2_ump*>(bytes + offset);
                auto sz = cmidi2_ump_get_message_size_bytes(msg);
                auto* words = reinterpret_cast<uint32_t*>(bytes + offset);
                words[0] = (words[0] & 0xF0FFFFFFu) | (static_cast<uint32_t>(group.value()) << 24);
                offset += sz;
            }
        }
    }
    if (!route->track->scheduleEvents(timestamp, ump, sizeInBytes))
        remidy::Logger::global()->logError(std::format("Failed to enqueue UMP events for target {}: size {}", targetId, sizeInBytes).c_str());
}

void uapmd::AudioPluginSequencer::enqueueUmpForInstance(int32_t instanceId, uapmd_ump_t *ump, size_t sizeInBytes, uapmd_timestamp_t timestamp) {
    auto mapping = plugin_function_blocks_.find(instanceId);
    if (mapping == plugin_function_blocks_.end() || !mapping->second.track)
        return;

    if (auto group = groupForInstanceOptional(instanceId); group.has_value()) {
        auto* bytes = reinterpret_cast<uint8_t*>(ump);
        size_t offset = 0;
        while (offset < sizeInBytes) {
            auto* msg = reinterpret_cast<cmidi2_ump*>(bytes + offset);
            auto sz = cmidi2_ump_get_message_size_bytes(msg);
            auto* words = reinterpret_cast<uint32_t*>(bytes + offset);
            words[0] = (words[0] & 0xF0FFFFFFu) | (static_cast<uint32_t>(group.value()) << 24);
            offset += sz;
        }
    }

    mapping->second.track->scheduleEvents(timestamp, ump, sizeInBytes);
}

void uapmd::AudioPluginSequencer::setPluginOutputHandler(int32_t instanceId, PluginOutputHandler handler) {
    auto current = std::atomic_load_explicit(&plugin_output_handlers_, std::memory_order_acquire);
    auto next = std::make_shared<HandlerMap>();
    if (current)
        *next = *current;
    if (handler) {
        (*next)[instanceId] = std::move(handler);
    } else {
        next->erase(instanceId);
    }
    std::atomic_store_explicit(&plugin_output_handlers_, next, std::memory_order_release);
}

void uapmd::AudioPluginSequencer::assignMidiDeviceToPlugin(int32_t instanceId, std::shared_ptr<MidiIODevice> device) {
    if (!device)
        return;
    auto* node = findPluginNodeByInstance(instanceId);
    if (!node)
        return;
    auto* palPtr = node->pal();
    if (!palPtr)
        return;
    auto mapper = std::make_unique<UapmdNodeUmpOutputMapper>(std::move(device), palPtr);
    node->setUmpOutputMapper(std::move(mapper));
}

void uapmd::AudioPluginSequencer::clearMidiDeviceFromPlugin(int32_t instanceId) {
    auto* node = findPluginNodeByInstance(instanceId);
    if (!node)
        return;
    node->setUmpOutputMapper(nullptr);
}

std::optional<uint8_t> uapmd::AudioPluginSequencer::pluginGroup(int32_t instanceId) const {
    return groupForInstanceOptional(instanceId);
}

std::optional<int32_t> uapmd::AudioPluginSequencer::instanceForGroup(uint8_t group) const {
    return instanceForGroupOptional(group);
}

uapmd_status_t uapmd::AudioPluginSequencer::startAudio() {
    return dispatcher->start();
}

uapmd_status_t uapmd::AudioPluginSequencer::stopAudio() {
    return dispatcher->stop();
}

uapmd_status_t uapmd::AudioPluginSequencer::isAudioPlaying() {
    return dispatcher->isPlaying();
}

void uapmd::AudioPluginSequencer::startPlayback() {
    sequencer->setPlaybackPosition(0);
    sequencer->setPlaybackActive(true);
}

void uapmd::AudioPluginSequencer::stopPlayback() {
    sequencer->setPlaybackActive(false);
    sequencer->setPlaybackPosition(0);
}

void uapmd::AudioPluginSequencer::pausePlayback() {
    sequencer->setPlaybackActive(false);
}

void uapmd::AudioPluginSequencer::resumePlayback() {
    sequencer->setPlaybackActive(true);
}

int64_t uapmd::AudioPluginSequencer::playbackPositionSamples() const {
    return sequencer->playbackPosition();
}

int32_t uapmd::AudioPluginSequencer::sampleRate() { return sample_rate; }
bool uapmd::AudioPluginSequencer::sampleRate(int32_t newSampleRate) {
    if (dispatcher->isPlaying())
        return false;
    sample_rate = newSampleRate;
    return true;
}

bool uapmd::AudioPluginSequencer::reconfigureAudioDevice(int inputDeviceIndex, int outputDeviceIndex, uint32_t sampleRate, uint32_t bufferSize) {
    // Stop audio if it's currently playing
    bool wasPlaying = dispatcher->isPlaying();
    if (wasPlaying) {
        if (stopAudio() != 0) {
            remidy::Logger::global()->logError("Failed to stop audio during device reconfiguration");
            return false;
        }
    }

    // Get a new audio device from the manager with specific device indices
    auto manager = AudioIODeviceManager::instance();
    auto* newDevice = manager->open(inputDeviceIndex, outputDeviceIndex, sampleRate);
    if (!newDevice) {
        remidy::Logger::global()->logError("Failed to open audio device with indices: input={}, output={}, sampleRate={}", inputDeviceIndex, outputDeviceIndex, sampleRate);
        return false;
    }

    // Update the sample rate if specified
    if (sampleRate > 0) {
        sample_rate = sampleRate;
    } else {
        // Use the device's actual sample rate
        sample_rate = static_cast<int32_t>(newDevice->sampleRate());
    }

    // Update the buffer size if specified
    if (bufferSize > 0) {
        buffer_size_in_frames = bufferSize;
        // Reconfigure all track contexts with the new buffer size
        auto& tracks = sequencer->tracks();
        auto& data = sequencer->data();
        for (size_t i = 0; i < tracks.size() && i < data.tracks.size(); i++) {
            auto* trackCtx = data.tracks[i];
            if (trackCtx) {
                // Get channel counts from the track's current configuration
                uint32_t inputChannels = trackCtx->audioInBusCount() > 0 ? trackCtx->inputChannelCount(0) : 2;
                uint32_t outputChannels = trackCtx->audioOutBusCount() > 0 ? trackCtx->outputChannelCount(0) : 2;

                // Reconfigure the track context with new buffer size
                // Note: This will recreate the audio buffers with the new size
                trackCtx->configureMainBus(inputChannels, outputChannels, buffer_size_in_frames);
            }
        }
    }

    // Reconfigure the dispatcher with the new device
    if (dispatcher->configure(ump_buffer_size_in_bytes, newDevice) != 0) {
        remidy::Logger::global()->logError("Failed to reconfigure dispatcher with new audio device");
        return false;
    }

    // Update SequencerEngine with new channel counts
    auto audioDevice = dispatcher->audio();
    const auto inputChannels = audioDevice ? std::max(audioDevice->inputChannels(), 2u) : 2;
    const auto outputChannels = audioDevice ? audioDevice->outputChannels() : 2;
    sequencer->setDefaultChannels(inputChannels, outputChannels);

    // Restart audio if it was playing before
    if (wasPlaying) {
        if (startAudio() != 0) {
            remidy::Logger::global()->logError("Failed to restart audio after device reconfiguration");
            return false;
        }
    }

    return true;
}

void uapmd::AudioPluginSequencer::loadAudioFile(std::unique_ptr<AudioFileReader> reader) {
    sequencer->loadAudioFile(std::move(reader));
}

void uapmd::AudioPluginSequencer::unloadAudioFile() {
    sequencer->unloadAudioFile();
}

double uapmd::AudioPluginSequencer::audioFileDurationSeconds() const {
    return sequencer->audioFileDurationSeconds();
}

void uapmd::AudioPluginSequencer::getInputSpectrum(float* outSpectrum, int numBars) const {
    sequencer->getInputSpectrum(outSpectrum, numBars);
}

void uapmd::AudioPluginSequencer::getOutputSpectrum(float* outSpectrum, int numBars) const {
    sequencer->getOutputSpectrum(outSpectrum, numBars);
}
