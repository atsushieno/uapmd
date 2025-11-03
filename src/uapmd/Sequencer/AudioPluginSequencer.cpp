
#include <atomic>
#include <cstring>
#include <format>
#include <iostream>
#include <optional>
#include <utility>
#include <cmidi2.h>
#include "uapmd/uapmd.hpp"
#include "uapmd/priv/sequencer/AudioPluginSequencer.hpp"
#include "uapmd/priv/plugingraph/AudioPluginHostPAL.hpp"

namespace {
    uapmd::AudioPluginHostPAL::AudioPluginNodePAL* findNodePalByInstanceId(
        uapmd::SequenceProcessor& sequencer,
        int32_t instanceId
    ) {
        for (auto& track : sequencer.tracks()) {
            for (auto node : track->graph().plugins()) {
                if (node->instanceId() == instanceId)
                    return node->pal();
            }
        }
        return nullptr;
    }
}

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

    size_t offset = 0;
    auto* byteView = reinterpret_cast<uint8_t*>(scratch);
    while (offset < bytes) {
        auto* msg = reinterpret_cast<cmidi2_ump*>(byteView + offset);
        auto size = cmidi2_ump_get_message_size_bytes(msg);
        auto* words = reinterpret_cast<uint32_t*>(byteView + offset);
        words[0] = (words[0] & 0x0FFFFFFF) | (static_cast<uint32_t>(group) << 28);
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
    int32_t sampleRate
) : buffer_size_in_frames(audioBufferSizeInFrames),
    ump_buffer_size_in_bytes(umpBufferSizeInBytes), sample_rate(sampleRate),
    plugin_host_pal(AudioPluginHostPAL::instance()),
    sequencer(sampleRate, buffer_size_in_frames, umpBufferSizeInBytes, plugin_host_pal),
    plugin_output_handlers_(std::make_shared<HandlerMap>()),
    plugin_output_scratch_(umpBufferSizeInBytes / sizeof(uapmd_ump_t), 0) {
    auto manager = AudioIODeviceManager::instance();
    auto logger = remidy::Logger::global();
    AudioIODeviceManager::Configuration audioConfig{ .logger = logger };
    manager->initialize(audioConfig);

    // FIXME: enable MIDI devices
    dispatcher.configure(umpBufferSizeInBytes, manager->open());

    dispatcher.addCallback([&](uapmd::AudioProcessContext& process) {
        auto& data = sequencer.data();

        for (uint32_t t = 0, nTracks = sequencer.tracks().size(); t < nTracks; t++) {
            if (t >= data.tracks.size())
                continue; // buffer not ready
            auto ctx = data.tracks[t];
            ctx->eventOut().position(0); // clean up *out* events here.
            ctx->frameCount(process.frameCount());
            for (uint32_t i = 0; i < process.audioInBusCount(); i++) {
                for (uint32_t ch = 0, nCh = process.inputChannelCount(i); ch < nCh; ch++)
                    memcpy(ctx->getFloatInBuffer(i, ch),
                           (void *) process.getFloatInBuffer(i, ch), process.frameCount() * sizeof(float));
            }
        }
        auto ret = sequencer.processAudio();

        for (uint32_t t = 0, nTracks = sequencer.tracks().size(); t < nTracks; t++) {
            if (t >= data.tracks.size())
                continue; // buffer not ready
            auto ctx = data.tracks[t];
            ctx->eventIn().position(0); // clean up *in* events here.
            for (uint32_t i = 0; i < process.audioOutBusCount(); i++) {
                for (uint32_t ch = 0, nCh = ctx->outputChannelCount(i); ch < nCh; ch++)
                    memcpy(process.getFloatOutBuffer(i, ch), (void*) ctx->getFloatOutBuffer(i, ch), process.frameCount() * sizeof(float));
            }
        }
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

std::vector<uapmd::ParameterMetadata> uapmd::AudioPluginSequencer::getParameterList(int32_t instanceId) {
    for (auto& track : sequencer.tracks())
        for (auto node : track->graph().plugins())
            if (node->instanceId() == instanceId)
                return node->pal()->parameterMetadataList();
    return {};
}

std::vector<uapmd::PresetsMetadata> uapmd::AudioPluginSequencer::getPresetList(int32_t instanceId) {
    for (auto& track : sequencer.tracks())
        for (auto node : track->graph().plugins())
            if (node->instanceId() == instanceId)
                return node->pal()->presetMetadataList();
    return {};
}

void uapmd::AudioPluginSequencer::loadPreset(int32_t instanceId, int32_t presetIndex) {
    for (auto& track : sequencer.tracks())
        for (auto node : track->graph().plugins())
            if (node->instanceId() == instanceId) {
                // Need to access the underlying plugin instance to call presets()->loadPreset()
                // This requires adding a method to the PAL interface
                node->pal()->loadPreset(presetIndex);
                return;
            }
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
    for (auto& track : sequencer.tracks()) {
        for (auto plugin : track->graph().plugins()) {
            if (plugin->instanceId() == instanceId) {
                // Get plugin ID and look it up in the catalog
                std::string pluginId = plugin->pal()->pluginId();
                std::string format = plugin->pal()->formatName();

                // Search in the catalog for display name
                auto plugins = plugin_host_pal->catalog().getPlugins();
                for (auto* catalogPlugin : plugins) {
                    if (catalogPlugin->pluginId() == pluginId && catalogPlugin->format() == format)
                        return catalogPlugin->displayName();
                }
                return "Plugin " + std::to_string(instanceId);
            }
        }
    }
    return "Unknown Plugin";
}

std::string uapmd::AudioPluginSequencer::getPluginFormat(int32_t instanceId) {
    for (auto& track : sequencer.tracks()) {
        for (auto plugin : track->graph().plugins()) {
            if (plugin->instanceId() == instanceId) {
                return plugin->pal()->formatName();
            }
        }
    }
    return "";
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

bool uapmd::AudioPluginSequencer::hasPluginUI(int32_t instanceId) {
    auto pal = findNodePalByInstanceId(sequencer, instanceId);
    if (!pal)
        return false;
    return pal->hasUISupport();
}

bool uapmd::AudioPluginSequencer::createPluginUI(int32_t instanceId, bool isFloating, void* parentHandle, std::function<bool(uint32_t, uint32_t)> resizeHandler) {
    auto pal = findNodePalByInstanceId(sequencer, instanceId);
    if (!pal)
        return false;
    return pal->createUI(isFloating, parentHandle, resizeHandler);
}

void uapmd::AudioPluginSequencer::destroyPluginUI(int32_t instanceId) {
    auto pal = findNodePalByInstanceId(sequencer, instanceId);
    if (!pal)
        return;
    pal->destroyUI();
}

bool uapmd::AudioPluginSequencer::showPluginUI(int32_t instanceId, bool isFloating, void* parentHandle) {
    auto pal = findNodePalByInstanceId(sequencer, instanceId);
    if (!pal)
        return false;
    // UI must be created via createPluginUI() first - just show it here
    return pal->showUI();
}

void uapmd::AudioPluginSequencer::hidePluginUI(int32_t instanceId) {
    auto pal = findNodePalByInstanceId(sequencer, instanceId);
    if (!pal)
        return;
    pal->hideUI();
}

bool uapmd::AudioPluginSequencer::isPluginUIVisible(int32_t instanceId) {
    auto pal = findNodePalByInstanceId(sequencer, instanceId);
    if (!pal)
        return false;
    return pal->isUIVisible();
}

bool uapmd::AudioPluginSequencer::resizePluginUI(int32_t instanceId, uint32_t width, uint32_t height) {
    auto pal = findNodePalByInstanceId(sequencer, instanceId);
    if (!pal)
        return false;
    return pal->setUISize(width, height);
}

bool uapmd::AudioPluginSequencer::getPluginUISize(int32_t instanceId, uint32_t &width, uint32_t &height) {
    auto pal = findNodePalByInstanceId(sequencer, instanceId);
    if (!pal)
        return false;
    return pal->getUISize(width, height);
}

bool uapmd::AudioPluginSequencer::canPluginUIResize(int32_t instanceId) {
    auto pal = findNodePalByInstanceId(sequencer, instanceId);
    if (!pal)
        return false;
    return pal->canUIResize();
}

void uapmd::AudioPluginSequencer::addSimplePluginTrack(
    std::string& format,
    std::string& pluginId,
    std::function<void(int32_t instanceId, std::string error)> callback
) {
    auto audioDevice = dispatcher.audio();
    const auto inputChannels = audioDevice ? audioDevice->inputChannels() : 0;
    const auto outputChannels = audioDevice ? audioDevice->outputChannels() : 0;
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

    auto audioDevice = dispatcher.audio();
    const auto inputChannels = audioDevice ? audioDevice->inputChannels() : 0;
    const auto outputChannels = audioDevice ? audioDevice->outputChannels() : 0;

    plugin_host_pal->createPluginInstance(sample_rate, inputChannels, outputChannels, format, pluginId,
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
    destroyPluginUI(instanceId);
    plugin_function_blocks_.erase(instanceId);
    releaseGroup(instanceId);
    setPluginOutputHandler(instanceId, nullptr);
    const auto removed = sequencer.removePluginInstance(instanceId);
    if (removed) {
        refreshFunctionBlockMappings();
    }
    return removed;
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
    cmidi2_ump umps[2];
    uint32_t vi32 = UINT32_MAX * value;
    auto ump = cmidi2_ump_midi2_nrpn(0, 0, (uint8_t) (index / 0x100), (uint8_t) (index % 0x100), vi32);
    addMessage64(umps, ump);
    for (auto& track : sequencer.tracks())
        for (auto& node : track->graph().plugins())
            if (node->instanceId() == instanceId) {
                node->pal()->setParameterValue(index, value);
                remidy::Logger::global()->logError(std::format("Native parameter change {}: {} = {}", instanceId, index, value).c_str());
                break;
            }
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

std::optional<uint8_t> uapmd::AudioPluginSequencer::pluginGroup(int32_t instanceId) const {
    return groupForInstanceOptional(instanceId);
}

std::optional<int32_t> uapmd::AudioPluginSequencer::instanceForGroup(uint8_t group) const {
    return instanceForGroupOptional(group);
}

uapmd_status_t uapmd::AudioPluginSequencer::startAudio() {
    return dispatcher.start();
}

uapmd_status_t uapmd::AudioPluginSequencer::stopAudio() {
    return dispatcher.stop();
}

uapmd_status_t uapmd::AudioPluginSequencer::isAudioPlaying() {
    return dispatcher.isPlaying();
}

int32_t uapmd::AudioPluginSequencer::sampleRate() { return sample_rate; }
bool uapmd::AudioPluginSequencer::sampleRate(int32_t newSampleRate) {
    if (dispatcher.isPlaying())
        return false;
    sample_rate = newSampleRate;
    return true;
}

void uapmd::AudioPluginSequencer::loadState(std::vector<uint8_t>& state) {
    // FIXME: we need some un-structure
    for (auto track : this->sequencer.tracks())
        for (auto plugin : track->graph().plugins())
            plugin->loadState(state);
}

std::vector<uint8_t> uapmd::AudioPluginSequencer::saveState() {
    std::vector<uint8_t> ret{};
    for (auto track : this->sequencer.tracks())
        for (auto plugin : track->graph().plugins()) {
            // FIXME: we need some structure
            auto target = plugin->saveState();
            std::copy(target.begin(), target.end(), std::back_inserter(ret));
        }
    return ret;
}
