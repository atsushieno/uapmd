
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
#include "uapmd/priv/plugingraph/AudioPluginHostPAL.hpp"
#include "../AudioPluginHosting/UapmdNodeUmpMapper.hpp"

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

void uapmd::AudioPluginSequencer::registerParameterListener(int32_t instanceId, AudioPluginHostPAL::AudioPluginNodePAL* node) {
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
    auto& tracks = sequencer.tracks();
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

    auto& tracks = sequencer.tracks();
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
    plugin_host_pal(AudioPluginHostPAL::instance()),
    sequencer(sampleRate, buffer_size_in_frames, umpBufferSizeInBytes, plugin_host_pal),
    plugin_output_handlers_(std::make_shared<HandlerMap>()),
    plugin_output_scratch_(umpBufferSizeInBytes / sizeof(uapmd_ump_t), 0) {
    auto manager = AudioIODeviceManager::instance();
    auto logger = remidy::Logger::global();
    AudioIODeviceManager::Configuration audioConfig{ .logger = logger };
    manager->initialize(audioConfig);

    // FIXME: enable MIDI devices
    dispatcher->configure(umpBufferSizeInBytes, manager->open());

    dispatcher->addCallback([&](uapmd::AudioProcessContext& process) {
        auto& data = sequencer.data();
        auto& masterContext = data.masterContext();

        // Update playback position if playback is active
        bool isPlaybackActive = is_playback_active_.load(std::memory_order_acquire);

        // Update MasterContext with current playback state
        masterContext.playbackPositionSamples(playback_position_samples_.load(std::memory_order_acquire));
        masterContext.isPlaying(isPlaybackActive);
        masterContext.sampleRate(sample_rate);

        // Prepare merged input buffer (audio file + mic input)
        std::vector<std::vector<float>> mergedInput;
        size_t audioFilePosition = 0;
        bool hasAudioFile = false;

        {
            std::lock_guard<std::mutex> lock(audio_file_mutex_);
            hasAudioFile = !audio_file_buffer_.empty();
            if (hasAudioFile && isPlaybackActive) {
                audioFilePosition = audio_file_read_position_.load(std::memory_order_acquire);
            }
        }

        // Determine number of channels for merged input
        // Use the maximum of device input channels and audio file channels
        uint32_t deviceInputChannels = process.audioInBusCount() > 0 ? process.inputChannelCount(0) : 0;
        uint32_t fileChannels = hasAudioFile ? audio_file_buffer_.size() : 0;
        uint32_t numInputChannels = std::max(deviceInputChannels, fileChannels);

        // If both are 0, default to stereo
        if (numInputChannels == 0) {
            numInputChannels = 2;
        }

        mergedInput.resize(numInputChannels);
        for (auto& channel : mergedInput) {
            channel.resize(process.frameCount(), 0.0f);
        }

        // Fill merged input buffer
        for (uint32_t ch = 0; ch < numInputChannels; ch++) {
            float* dst = mergedInput[ch].data();

            // Start with device input (mic)
            if (process.audioInBusCount() > 0 && ch < process.inputChannelCount(0)) {
                memcpy(dst, (void*)process.getFloatInBuffer(0, ch), process.frameCount() * sizeof(float));
            }

            // Add audio file playback if available and playing
            if (hasAudioFile && isPlaybackActive) {
                std::lock_guard<std::mutex> lock(audio_file_mutex_);
                if (ch < audio_file_buffer_.size()) {
                    const auto& channelData = audio_file_buffer_[ch];
                    for (uint32_t frame = 0; frame < process.frameCount(); ++frame) {
                        size_t pos = audioFilePosition + frame;
                        if (pos < channelData.size()) {
                            dst[frame] += channelData[pos];
                        }
                    }
                }
            }
        }

        // Send merged input to ALL tracks
        for (uint32_t t = 0, nTracks = sequencer.tracks().size(); t < nTracks; t++) {
            if (t >= data.tracks.size())
                continue; // buffer not ready
            auto ctx = data.tracks[t];
            ctx->eventOut().position(0); // clean up *out* events here.
            ctx->frameCount(process.frameCount());

            // Copy merged input to track input buffers
            for (uint32_t i = 0; i < ctx->audioInBusCount(); i++) {
                for (uint32_t ch = 0, nCh = ctx->inputChannelCount(i); ch < nCh; ch++) {
                    float* trackDst = ctx->getFloatInBuffer(i, ch);
                    if (ch < mergedInput.size()) {
                        memcpy(trackDst, mergedInput[ch].data(), process.frameCount() * sizeof(float));
                    } else {
                        memset(trackDst, 0, process.frameCount() * sizeof(float));
                    }
                }
            }
        }

        // Advance audio file read position only when playing
        if (hasAudioFile && isPlaybackActive) {
            audio_file_read_position_.fetch_add(process.frameCount(), std::memory_order_release);
        }

        auto ret = sequencer.processAudio();

        // Clear main output bus (bus 0) before mixing
        if (process.audioOutBusCount() > 0) {
            for (uint32_t ch = 0; ch < process.outputChannelCount(0); ch++) {
                memset(process.getFloatOutBuffer(0, ch), 0, process.frameCount() * sizeof(float));
            }
        }

        // Mix all tracks into main output bus with additive mixing
        for (uint32_t t = 0, nTracks = sequencer.tracks().size(); t < nTracks; t++) {
            if (t >= data.tracks.size())
                continue; // buffer not ready
            auto ctx = data.tracks[t];
            ctx->eventIn().position(0); // clean up *in* events here.

            // Mix only main bus (bus 0)
            if (process.audioOutBusCount() > 0 && ctx->audioOutBusCount() > 0) {
                // Mix matching channels only
                uint32_t numChannels = std::min(ctx->outputChannelCount(0), process.outputChannelCount(0));
                for (uint32_t ch = 0; ch < numChannels; ch++) {
                    float* dst = process.getFloatOutBuffer(0, ch);
                    const float* src = ctx->getFloatOutBuffer(0, ch);
                    // Additive mixing
                    for (uint32_t frame = 0; frame < process.frameCount(); frame++) {
                        dst[frame] += src[frame];
                    }
                }
            }
        }

        // Apply soft clipping to prevent harsh distortion
        if (process.audioOutBusCount() > 0) {
            for (uint32_t ch = 0; ch < process.outputChannelCount(0); ch++) {
                float* buffer = process.getFloatOutBuffer(0, ch);
                for (uint32_t frame = 0; frame < process.frameCount(); frame++) {
                    buffer[frame] = std::tanh(buffer[frame]);
                }
            }
        }

        // Calculate spectrum for visualization (simple magnitude binning)
        {
            std::lock_guard<std::mutex> lock(spectrum_mutex_);

            // Calculate input spectrum from merged input buffer
            for (int bar = 0; bar < kSpectrumBars; ++bar) {
                float sum = 0.0f;
                int sampleCount = 0;

                if (!mergedInput.empty()) {
                    int samplesPerBar = process.frameCount() / kSpectrumBars;
                    int startSample = bar * samplesPerBar;
                    int endSample = std::min((int)process.frameCount(), (bar + 1) * samplesPerBar);

                    for (uint32_t ch = 0; ch < mergedInput.size(); ++ch) {
                        const float* buffer = mergedInput[ch].data();
                        for (int i = startSample; i < endSample; ++i) {
                            sum += std::abs(buffer[i]);
                            sampleCount++;
                        }
                    }
                }

                input_spectrum_[bar] = sampleCount > 0 ? sum / sampleCount : 0.0f;
            }

            // Calculate output spectrum from main output
            for (int bar = 0; bar < kSpectrumBars; ++bar) {
                float sum = 0.0f;
                int sampleCount = 0;

                if (process.audioOutBusCount() > 0) {
                    int samplesPerBar = process.frameCount() / kSpectrumBars;
                    int startSample = bar * samplesPerBar;
                    int endSample = std::min((int)process.frameCount(), (bar + 1) * samplesPerBar);

                    for (uint32_t ch = 0; ch < process.outputChannelCount(0); ++ch) {
                        const float* buffer = process.getFloatOutBuffer(0, ch);
                        for (int i = startSample; i < endSample; ++i) {
                            sum += std::abs(buffer[i]);
                            sampleCount++;
                        }
                    }
                }

                output_spectrum_[bar] = sampleCount > 0 ? sum / sampleCount : 0.0f;
            }
        }

        if (isPlaybackActive)
            playback_position_samples_.fetch_add(process.frameCount(), std::memory_order_release);

        return ret;
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
    for (auto& track : sequencer.tracks()) {
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

    auto& tracksRef = sequencer.tracks();
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
    auto& tracksRef = sequencer.tracks();
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
    auto& tracksRef = sequencer.tracks();
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
    auto audioDevice = dispatcher->audio();
    // Always use at least 2 input channels to support audio file playback even without mic input
    const auto inputChannels = audioDevice ? std::max(audioDevice->inputChannels(), 2u) : 2;
    const auto outputChannels = audioDevice ? audioDevice->outputChannels() : 2;
    sequencer.addSimpleTrack(format, pluginId, inputChannels, outputChannels, [&,callback,inputChannels,outputChannels](AudioPluginTrack* track, std::string error) {
        if (!error.empty()) {
            callback(-1, error);
        } else {
            auto trackCtx = sequencer.data().tracks[sequencer.tracks().size() - 1];
            trackCtx->configureMainBus(inputChannels, outputChannels, buffer_size_in_frames);

            configureTrackRouting(track);
            auto trackIndex = static_cast<int32_t>(sequencer.tracks().size() - 1);
            auto plugins = track->graph().plugins();
            if (plugins.empty()) {
                callback(-1, "Track has no plugins after instantiation");
                return;
            }
            auto* plugin = plugins.front();
            auto instanceId = plugin->instanceId();
            plugin_function_blocks_[instanceId] = FunctionBlockRoute{track, trackIndex};
            assignGroup(instanceId);
            AudioPluginHostPAL::AudioPluginNodePAL* palPtr = nullptr;
            {
                std::lock_guard<std::mutex> lock(instance_map_mutex_);
                palPtr = plugin->pal();
                plugin_instances_[instanceId] = palPtr;
                plugin_bypassed_[instanceId] = false;
            }

            registerParameterListener(instanceId, palPtr);

            refreshFunctionBlockMappings();
            callback(instanceId, error);
        }
    });
}

void uapmd::AudioPluginSequencer::addPluginToTrack(
    int32_t trackIndex,
    std::string& format,
    std::string& pluginId,
    std::function<void(int32_t instanceId, std::string error)> callback
) {
    if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= sequencer.tracks().size()) {
        callback(-1, std::format("Invalid track index {}", trackIndex));
        return;
    }

    auto audioDevice = dispatcher->audio();
    // Always use at least 2 input channels to support audio file playback even without mic input
    const auto inputChannels = audioDevice ? std::max(audioDevice->inputChannels(), 2u) : 2;
    const auto outputChannels = audioDevice ? audioDevice->outputChannels() : 2;

    plugin_host_pal->createPluginInstance(sample_rate, inputChannels, outputChannels, offline_rendering_.load(std::memory_order_acquire), format, pluginId,
        [this, trackIndex, cb = std::move(callback)](auto node, std::string error) mutable {
            if (!node) {
                if (cb) {
                    cb(-1, "Could not create plugin: " + error);
                }
                return;
            }

            auto& tracksRef = sequencer.tracks();
            if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= tracksRef.size()) {
                if (cb) {
                    cb(-1, std::format("Track {} no longer exists", trackIndex));
                }
                return;
            }

            auto* track = tracksRef[static_cast<size_t>(trackIndex)];
            auto status = track->graph().appendNodeSimple(std::move(node));
            if (status != 0) {
                if (cb) {
                    cb(-1, std::format("Failed to append plugin to track {} (status {})", trackIndex, status));
                }
                return;
            }

            auto plugins = track->graph().plugins();
            if (plugins.empty()) {
                if (cb) {
                    cb(-1, "Track has no plugins after append");
                }
                return;
            }

            configureTrackRouting(track);
            auto* appended = plugins.back();
            if (appended) {
                auto instanceId = appended->instanceId();
                plugin_function_blocks_[instanceId] = FunctionBlockRoute{track, trackIndex};
                assignGroup(instanceId);
                AudioPluginHostPAL::AudioPluginNodePAL* palPtr = nullptr;
                {
                    std::lock_guard<std::mutex> lock(instance_map_mutex_);
                    palPtr = appended->pal();
                    plugin_instances_[instanceId] = palPtr;
                    plugin_bypassed_[instanceId] = false;
                }
                registerParameterListener(instanceId, palPtr);
                refreshFunctionBlockMappings();
                if (cb) {
                    cb(instanceId, "");
                }
            } else if (cb) {
                cb(-1, "Appended plugin could not be resolved");
            }
        });
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
    const auto removed = sequencer.removePluginInstance(instanceId);
    if (removed) {
        refreshFunctionBlockMappings();
    }
    return removed;
}

uapmd::AudioPluginHostPAL::AudioPluginNodePAL* uapmd::AudioPluginSequencer::getPluginInstance(int32_t instanceId) {
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
    playback_position_samples_.store(0, std::memory_order_release);
    audio_file_read_position_.store(0, std::memory_order_release);
    is_playback_active_.store(true, std::memory_order_release);
}

void uapmd::AudioPluginSequencer::stopPlayback() {
    is_playback_active_.store(false, std::memory_order_release);
    playback_position_samples_.store(0, std::memory_order_release);
    audio_file_read_position_.store(0, std::memory_order_release);
}

void uapmd::AudioPluginSequencer::pausePlayback() {
    is_playback_active_.store(false, std::memory_order_release);
}

void uapmd::AudioPluginSequencer::resumePlayback() {
    is_playback_active_.store(true, std::memory_order_release);
}

int64_t uapmd::AudioPluginSequencer::playbackPositionSamples() const {
    return playback_position_samples_.load(std::memory_order_acquire);
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
        auto& tracks = sequencer.tracks();
        auto& data = sequencer.data();
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

    // Restart audio if it was playing before
    if (wasPlaying) {
        if (startAudio() != 0) {
            remidy::Logger::global()->logError("Failed to restart audio after device reconfiguration");
            return false;
        }
    }

    return true;
}

void uapmd::AudioPluginSequencer::loadAudioFile(std::unique_ptr<choc::audio::AudioFileReader> reader) {
    std::lock_guard<std::mutex> lock(audio_file_mutex_);

    audio_file_reader_ = std::move(reader);
    audio_file_read_position_.store(0, std::memory_order_release);

    if (!audio_file_reader_) {
        audio_file_buffer_.clear();
        return;
    }

    // Load entire file into memory for simplicity
    const auto& props = audio_file_reader_->getProperties();
    auto numFrames = props.numFrames;
    auto numChannels = props.numChannels;

    audio_file_buffer_.resize(numChannels);
    for (auto& channel : audio_file_buffer_) {
        channel.resize(numFrames);
    }

    // Read all audio data using ChannelArrayBuffer
    choc::buffer::ChannelArrayBuffer<float> tempBuffer(numChannels, numFrames);
    audio_file_reader_->readFrames(0, tempBuffer.getView());

    // Copy to our planar format
    for (uint32_t ch = 0; ch < numChannels; ++ch) {
        for (uint64_t frame = 0; frame < numFrames; ++frame) {
            audio_file_buffer_[ch][frame] = tempBuffer.getSample(ch, frame);
        }
    }
}

void uapmd::AudioPluginSequencer::unloadAudioFile() {
    std::lock_guard<std::mutex> lock(audio_file_mutex_);
    audio_file_reader_.reset();
    audio_file_buffer_.clear();
    audio_file_read_position_.store(0, std::memory_order_release);
}

double uapmd::AudioPluginSequencer::audioFileDurationSeconds() const {
    std::lock_guard<std::mutex> lock(audio_file_mutex_);
    if (!audio_file_reader_)
        return 0.0;
    const auto& props = audio_file_reader_->getProperties();
    return static_cast<double>(props.numFrames) / props.sampleRate;
}

void uapmd::AudioPluginSequencer::getInputSpectrum(float* outSpectrum, int numBars) const {
    std::lock_guard<std::mutex> lock(spectrum_mutex_);
    for (int i = 0; i < std::min(numBars, kSpectrumBars); ++i) {
        outSpectrum[i] = input_spectrum_[i];
    }
}

void uapmd::AudioPluginSequencer::getOutputSpectrum(float* outSpectrum, int numBars) const {
    std::lock_guard<std::mutex> lock(spectrum_mutex_);
    for (int i = 0; i < std::min(numBars, kSpectrumBars); ++i) {
        outSpectrum[i] = output_spectrum_[i];
    }
}
