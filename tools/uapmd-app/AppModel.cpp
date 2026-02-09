
#include "uapmd/uapmd.hpp"
#include "AppModel.hpp"
#include <uapmd-data/priv/project/Smf2ClipReader.hpp>
#include <uapmd-data/priv/timeline/MidiClipSourceNode.hpp>
#include <umppi/umppi.hpp>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <limits>
#include <exception>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define DEFAULT_AUDIO_BUFFER_SIZE 1024
#define DEFAULT_UMP_BUFFER_SIZE 65536
#define DEFAULT_SAMPLE_RATE 48000
#define FIXED_CHANNEL_COUNT 2

std::unique_ptr<uapmd::AppModel> model{};

namespace {

constexpr uint32_t kDefaultDctpq = 480;
constexpr uint8_t kTempoGroup = 0;
constexpr uint8_t kTempoChannel = 0;

struct ScheduledUmp {
    uint64_t tick{0};
    int priority{0};
    umppi::Ump message{};
};

struct GatheredClipEvents {
    std::vector<ScheduledUmp> events;
    uint64_t endTick{0};
};

std::filesystem::path makeRelativePath(const std::filesystem::path& baseDir, const std::filesystem::path& target) {
    if (baseDir.empty() || target.empty())
        return target;

    std::error_code ec;
    auto rel = std::filesystem::relative(target, baseDir, ec);
    if (ec)
        return target;

    for (const auto& part : rel) {
        if (part == "..")
            return target;
    }
    return rel;
}

std::filesystem::path makeAbsolutePath(const std::filesystem::path& baseDir, const std::filesystem::path& target) {
    if (target.empty())
        return target;
    if (target.is_absolute() || baseDir.empty())
        return std::filesystem::absolute(target);
    return std::filesystem::absolute(baseDir / target);
}

uint32_t bpmToTenNanoseconds(double bpm) {
    double clampedBpm = std::clamp(bpm, 0.0001, 960.0);
    double value = 6000000000.0 / clampedBpm;
    value = std::clamp(value, 1.0, static_cast<double>(std::numeric_limits<uint32_t>::max()));
    return static_cast<uint32_t>(value);
}

uint64_t secondsToTicks(double seconds, double bpm, uint32_t ticksPerQuarter) {
    if (seconds <= 0.0)
        return 0;
    double beatsPerSecond = std::max(0.0001, bpm) / 60.0;
    double ticksPerSecond = beatsPerSecond * static_cast<double>(ticksPerQuarter);
    double ticks = seconds * ticksPerSecond;
    if (!std::isfinite(ticks) || ticks < 0.0)
        return 0;
    return static_cast<uint64_t>(std::llround(ticks));
}

class SerializedProjectPluginGraph final : public uapmd::UapmdProjectPluginGraphData {
public:
    std::filesystem::path external_file;
    std::vector<uapmd::UapmdProjectPluginNodeData> nodes;

    std::filesystem::path externalFile() override { return external_file; }
    std::vector<uapmd::UapmdProjectPluginNodeData> plugins() override { return nodes; }
    void externalFile(const std::filesystem::path& f) override { external_file = f; }

    void setPlugins(std::vector<uapmd::UapmdProjectPluginNodeData>&& newNodes) {
        nodes = std::move(newNodes);
    }
};

std::unique_ptr<uapmd::UapmdProjectPluginGraphData> createSerializedPluginGraph(
    uapmd::SequencerTrack* sequencerTrack,
    uapmd::SequencerEngine* engine)
{
    if (!sequencerTrack)
        return nullptr;

    std::vector<uapmd::UapmdProjectPluginNodeData> pluginNodes;
    const auto& orderedIds = sequencerTrack->orderedInstanceIds();
    pluginNodes.reserve(orderedIds.size());

    for (int32_t instanceId : orderedIds) {
        if (instanceId < 0)
            continue;

        uapmd::AudioPluginInstanceAPI* instance = nullptr;
        if (auto* node = sequencerTrack->graph().getPluginNode(instanceId))
            instance = node->instance();
        if (!instance && engine)
            instance = engine->getPluginInstance(instanceId);
        if (!instance)
            continue;

        uapmd::UapmdProjectPluginNodeData nodeData;
        nodeData.plugin_id = instance->pluginId();
        nodeData.format = instance->formatName();
        pluginNodes.push_back(std::move(nodeData));
    }

    auto graph = std::make_unique<SerializedProjectPluginGraph>();
    graph->setPlugins(std::move(pluginNodes));
    return graph;
}

GatheredClipEvents gatherMidiClipEvents(const uapmd::MidiClipSourceNode& node) {
    GatheredClipEvents result;
    const auto& eventWords = node.umpEvents();
    const auto& eventTicks = node.eventTimestampsTicks();
    const auto& tempoChanges = node.tempoChanges();
    const auto& timeSigChanges = node.timeSignatureChanges();

    auto priorityFor = [](const umppi::Ump& message) {
        if (message.isTempo())
            return 0;
        if (message.isTimeSignature())
            return 1;
        return 2;
    };

    std::unordered_set<uint64_t> tempoTicks;
    std::unordered_set<uint64_t> timeSigTicks;

    size_t wordIndex = 0;
    auto readNextUmp = [&](umppi::Ump& message, uint64_t& eventTick) -> bool {
        if (wordIndex >= eventWords.size())
            return false;
        const uint32_t word = eventWords[wordIndex];
        const auto messageType = static_cast<uint8_t>((word >> 28) & 0xF);
        const int wordCount = umppi::umpSizeInInts(messageType);
        if (wordCount <= 0)
            return false;
        if (wordIndex + static_cast<size_t>(wordCount) > eventWords.size())
            return false;

        switch (wordCount) {
            case 1:
                message = umppi::Ump(word);
                break;
            case 2:
                message = umppi::Ump(word, eventWords[wordIndex + 1]);
                break;
            case 4:
                message = umppi::Ump(
                    word,
                    eventWords[wordIndex + 1],
                    eventWords[wordIndex + 2],
                    eventWords[wordIndex + 3]);
                break;
            default:
                return false;
        }

        eventTick = (!eventTicks.empty() && wordIndex < eventTicks.size())
            ? eventTicks[wordIndex]
            : 0;
        wordIndex += static_cast<size_t>(wordCount);
        return true;
    };

    bool reachedEnd = false;
    while (wordIndex < eventWords.size()) {
        umppi::Ump message;
        uint64_t absoluteTick = 0;
        if (!readNextUmp(message, absoluteTick))
            break;

        if (message.isDeltaClockstamp() || message.isDCTPQ() || message.isStartOfClip())
            continue;

        if (message.isEndOfClip()) {
            result.endTick = std::max(result.endTick, absoluteTick);
            reachedEnd = true;
            break;
        }

        ScheduledUmp entry;
        entry.tick = absoluteTick;
        entry.priority = priorityFor(message);
        entry.message = message;
        result.events.push_back(entry);
        if (entry.tick > result.endTick)
            result.endTick = entry.tick;

        if (message.isTempo())
            tempoTicks.insert(entry.tick);
        else if (message.isTimeSignature())
            timeSigTicks.insert(entry.tick);
    }

    if (!reachedEnd && !eventTicks.empty())
        result.endTick = std::max(result.endTick, eventTicks.back());

    for (const auto& tempo : tempoChanges) {
        if (tempoTicks.contains(tempo.tickPosition))
            continue;

        ScheduledUmp entry;
        entry.tick = tempo.tickPosition;
        entry.priority = 0;
        const double bpm = tempo.bpm > 0.0 ? tempo.bpm : 120.0;
        entry.message = umppi::UmpFactory::tempo(
            kTempoGroup,
            kTempoChannel,
            bpmToTenNanoseconds(bpm));
        result.events.push_back(entry);
        tempoTicks.insert(entry.tick);
        if (entry.tick > result.endTick)
            result.endTick = entry.tick;
    }

    for (const auto& sig : timeSigChanges) {
        if (timeSigTicks.contains(sig.tickPosition))
            continue;

        ScheduledUmp entry;
        entry.tick = sig.tickPosition;
        entry.priority = 1;
        entry.message = umppi::UmpFactory::timeSignatureDirect(
            kTempoGroup,
            kTempoChannel,
            sig.numerator,
            sig.denominator,
            0);
        result.events.push_back(entry);
        timeSigTicks.insert(entry.tick);
        if (entry.tick > result.endTick)
            result.endTick = entry.tick;
    }

    std::stable_sort(result.events.begin(), result.events.end(), [](const ScheduledUmp& a, const ScheduledUmp& b) {
        if (a.tick != b.tick)
            return a.tick < b.tick;
        return a.priority < b.priority;
    });

    if (!result.events.empty() && result.events.back().tick > result.endTick)
        result.endTick = result.events.back().tick;

    return result;
}

std::vector<umppi::Ump> buildSmf2ClipFromMidiNode(const uapmd::MidiClipSourceNode& node) {
    std::vector<umppi::Ump> clip;
    clip.emplace_back(umppi::Ump(umppi::UmpFactory::deltaClockstamp(0)));
    clip.emplace_back(umppi::Ump(umppi::UmpFactory::dctpq(node.tickResolution())));
    clip.emplace_back(umppi::Ump(umppi::UmpFactory::deltaClockstamp(0)));
    clip.push_back(umppi::UmpFactory::startOfClip());

    auto gathered = gatherMidiClipEvents(node);
    uint64_t previousTick = 0;
    for (const auto& entry : gathered.events) {
        uint64_t delta = entry.tick >= previousTick ? entry.tick - previousTick : 0;
        clip.emplace_back(umppi::Ump(umppi::UmpFactory::deltaClockstamp(static_cast<uint32_t>(delta))));
        clip.push_back(entry.message);
        previousTick = entry.tick;
    }

    uint64_t tailDelta = gathered.endTick > previousTick ? gathered.endTick - previousTick : 0;
    clip.emplace_back(umppi::Ump(umppi::UmpFactory::deltaClockstamp(static_cast<uint32_t>(tailDelta))));
    clip.push_back(umppi::UmpFactory::endOfClip());

    return clip;
}

std::vector<umppi::Ump> buildMasterTrackSmf2Clip(const uapmd::AppModel::MasterTrackSnapshot& snapshot) {
    std::vector<umppi::Ump> clip;
    clip.emplace_back(umppi::Ump(umppi::UmpFactory::deltaClockstamp(0)));
    clip.emplace_back(umppi::Ump(umppi::UmpFactory::dctpq(kDefaultDctpq)));
    clip.emplace_back(umppi::Ump(umppi::UmpFactory::deltaClockstamp(0)));
    clip.push_back(umppi::UmpFactory::startOfClip());

    struct MasterEvent {
        double timeSeconds{0.0};
        int priority{0};
        umppi::Ump message{};
    };

    std::vector<MasterEvent> events;
    events.reserve(snapshot.tempoPoints.size() + snapshot.timeSignaturePoints.size());
    for (const auto& tempo : snapshot.tempoPoints) {
        MasterEvent evt;
        evt.timeSeconds = tempo.timeSeconds;
        evt.priority = 0;
        evt.message = umppi::UmpFactory::tempo(kTempoGroup, kTempoChannel, bpmToTenNanoseconds(tempo.bpm));
        events.push_back(evt);
    }
    for (const auto& sig : snapshot.timeSignaturePoints) {
        MasterEvent evt;
        evt.timeSeconds = sig.timeSeconds;
        evt.priority = 1;
        evt.message = umppi::UmpFactory::timeSignatureDirect(
            kTempoGroup,
            kTempoChannel,
            sig.signature.numerator,
            sig.signature.denominator,
            0);
        events.push_back(evt);
    }

    std::sort(events.begin(), events.end(), [](const MasterEvent& a, const MasterEvent& b) {
        if (std::abs(a.timeSeconds - b.timeSeconds) > 1e-9)
            return a.timeSeconds < b.timeSeconds;
        return a.priority < b.priority;
    });

    double currentTempo = 120.0;
    double currentTime = 0.0;
    uint64_t currentTick = 0;
    for (const auto& evt : events) {
        double deltaSeconds = std::max(0.0, evt.timeSeconds - currentTime);
        uint64_t deltaTicks = secondsToTicks(deltaSeconds, currentTempo, kDefaultDctpq);
        currentTick += deltaTicks;
        clip.emplace_back(umppi::Ump(umppi::UmpFactory::deltaClockstamp(static_cast<uint32_t>(deltaTicks))));
        clip.push_back(evt.message);
        currentTime = evt.timeSeconds;
        if (evt.priority == 0) {
            double bpm = 120.0;
            uint32_t tempoVal = evt.message.getTempo();
            if (tempoVal > 0) {
                bpm = 6000000000.0 / static_cast<double>(tempoVal);
            }
            currentTempo = std::max(0.1, bpm);
        }
    }

    clip.emplace_back(umppi::Ump(umppi::UmpFactory::deltaClockstamp(0)));
    clip.push_back(umppi::UmpFactory::endOfClip());

    return clip;
}

struct ParsedSmf2Clip {
    uint32_t tickResolution{kDefaultDctpq};
    std::vector<uapmd_ump_t> events;
    std::vector<uint64_t> eventTicks;
    std::vector<uapmd::MidiTempoChange> tempoChanges;
    std::vector<uapmd::MidiTimeSignatureChange> timeSignatureChanges;
};

bool parseSmf2ClipFile(const std::filesystem::path& file, ParsedSmf2Clip& parsed, std::string& error) {
    auto clip = uapmd::Smf2ClipReaderWriter::read(file, &error);
    if (!clip)
        return false;
    if (clip->size() < 4) {
        error = "SMF2 clip is incomplete";
        return false;
    }

    auto it = clip->begin();
    if (!it->isDeltaClockstamp() || it->getDeltaClockstamp() != 0) {
        error = "SMF2 clip missing initial DeltaClockstamp(0)";
        return false;
    }
    ++it;

    if (!it->isDCTPQ()) {
        error = "SMF2 clip missing DCTPQ after header";
        return false;
    }
    parsed.tickResolution = it->getDCTPQ();
    ++it;

    if (!it->isDeltaClockstamp() || it->getDeltaClockstamp() != 0) {
        error = "SMF2 clip missing DeltaClockstamp before StartOfClip";
        return false;
    }
    ++it;

    if (!it->isStartOfClip()) {
        error = "SMF2 clip missing StartOfClip marker";
        return false;
    }
    ++it;

    uint64_t currentTick = 0;
    bool expectDelta = true;
    bool endOfClipSeen = false;
    for (; it != clip->end(); ++it) {
        if (expectDelta) {
            if (!it->isDeltaClockstamp()) {
                error = "SMF2 clip missing delta clockstamp";
                return false;
            }
            currentTick += it->getDeltaClockstamp();
            expectDelta = false;
        } else {
            if (it->isEndOfClip()) {
                auto after = it;
                ++after;
                if (after != clip->end()) {
                    error = "EndOfClip must be the final event in SMF2 clip";
                    return false;
                }
                endOfClipSeen = true;
                expectDelta = true;
                break;
            } else if (it->isTempo()) {
                double bpm = 120.0;
                uint32_t rawTempo = it->getTempo();
                if (rawTempo > 0) {
                    bpm = 6000000000.0 / static_cast<double>(rawTempo);
                }
                parsed.tempoChanges.push_back(uapmd::MidiTempoChange{currentTick, bpm});
            } else if (it->isTimeSignature()) {
                uapmd::MidiTimeSignatureChange sig{};
                sig.tickPosition = currentTick;
                sig.numerator = it->getTimeSignatureNumerator();
                sig.denominator = it->getTimeSignatureDenominator();
                parsed.timeSignatureChanges.push_back(sig);
            } else {
                parsed.events.push_back(static_cast<uapmd_ump_t>(it->int1));
                parsed.eventTicks.push_back(currentTick);
            }
            expectDelta = true;
        }
    }

    if (!endOfClipSeen) {
        error = "SMF2 clip missing EndOfClip marker";
        return false;
    }

    if (!expectDelta) {
        error = "Dangling delta clockstamp without event";
        return false;
    }

    return true;
}

} // namespace
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
    const bool targetMasterTrack = (trackIndex == kMasterTrackIndex);
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

    if (!targetMasterTrack) {
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
    }

    sequencer_.engine()->addPluginToTrack(targetMasterTrack ? kMasterTrackIndex : trackIndex, formatCopy, pluginIdCopy, instantiateCallback);
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
        SequencerTrack* targetTrack = nullptr;
        const auto targetIndex = sequencer_.engine()->findTrackIndexForInstance(instanceId);
        if (targetIndex == kMasterTrackIndex) {
            targetTrack = sequencer_.engine()->masterTrack();
        } else if (targetIndex >= 0) {
            auto& trackRefs = sequencer_.engine()->tracks();
            if (static_cast<size_t>(targetIndex) < trackRefs.size()) {
                targetTrack = trackRefs[static_cast<size_t>(targetIndex)];
            }
        }
        const auto pluginNode = targetTrack ? targetTrack->graph().getPluginNode(instanceId) : nullptr;
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
            static_cast<double>(sample_rate_),
            std::move(clipInfo.tempo_changes),
            std::move(clipInfo.time_signature_changes)
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

uapmd::AppModel::ClipAddResult uapmd::AppModel::addMidiClipToTrack(
    int32_t trackIndex,
    const uapmd::TimelinePosition& position,
    std::vector<uapmd_ump_t> umpEvents,
    std::vector<uint64_t> umpTickTimestamps,
    uint32_t tickResolution,
    double clipTempo,
    std::vector<MidiTempoChange> tempoChanges,
    std::vector<MidiTimeSignatureChange> timeSignatureChanges,
    const std::string& clipName
) {
    ClipAddResult result;

    if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(timeline_tracks_.size())) {
        result.error = "Invalid track index";
        return result;
    }

    try {
        // Create MIDI source node with provided UMP data
        int32_t sourceNodeId = next_source_node_id_++;
        auto sourceNode = std::make_unique<uapmd::MidiClipSourceNode>(
            sourceNodeId,
            std::move(umpEvents),
            std::move(umpTickTimestamps),
            tickResolution,
            clipTempo,
            static_cast<double>(sample_rate_),
            std::move(tempoChanges),
            std::move(timeSignatureChanges)
        );

        // Get duration from source node
        int64_t durationSamples = sourceNode->totalLength();

        // Create MIDI clip data
        uapmd::ClipData clip;
        clip.clipType = uapmd::ClipType::Midi;
        clip.position = position;
        clip.durationSamples = durationSamples;
        clip.sourceNodeInstanceId = sourceNodeId;
        clip.filepath = "";  // No file associated with programmatically created clips
        clip.tickResolution = tickResolution;
        clip.clipTempo = clipTempo;
        clip.gain = 1.0;
        clip.muted = false;
        clip.name = clipName.empty() ? "MIDI Clip" : clipName;

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
        result.error = std::format("Failed to create MIDI clip: {}", ex.what());
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

uapmd::AppModel::MasterTrackSnapshot uapmd::AppModel::buildMasterTrackSnapshot() {
    MasterTrackSnapshot snapshot;
    const double sampleRate = std::max(1.0, static_cast<double>(sample_rate_));

    for (const auto& trackPtr : timeline_tracks_) {
        if (!trackPtr)
            continue;

        auto clips = trackPtr->clipManager().getAllClips();
        if (clips.empty())
            continue;

        std::unordered_map<int32_t, const uapmd::ClipData*> clipMap;
        clipMap.reserve(clips.size());
        for (auto* clip : clips) {
            if (clip)
                clipMap[clip->clipId] = clip;
        }

        for (auto* clip : clips) {
            if (!clip || clip->clipType != uapmd::ClipType::Midi)
                continue;

            auto* sourceNode = trackPtr->getSourceNode(clip->sourceNodeInstanceId);
            auto* midiNode = dynamic_cast<MidiClipSourceNode*>(sourceNode);
            if (!midiNode)
                continue;

            const auto absolutePosition = clip->getAbsolutePosition(clipMap);
            const double clipStartSamples = static_cast<double>(absolutePosition.samples);

            const auto& tempoSamples = midiNode->tempoChangeSamples();
            const auto& tempoEvents = midiNode->tempoChanges();
            const size_t tempoCount = std::min(tempoSamples.size(), tempoEvents.size());
            for (size_t i = 0; i < tempoCount; ++i) {
                MasterTrackSnapshot::TempoPoint point;
                point.timeSeconds = (clipStartSamples + static_cast<double>(tempoSamples[i])) / sampleRate;
                point.bpm = tempoEvents[i].bpm;
                snapshot.maxTimeSeconds = std::max(snapshot.maxTimeSeconds, point.timeSeconds);
                snapshot.tempoPoints.push_back(point);
            }

            const auto& sigSamples = midiNode->timeSignatureChangeSamples();
            const auto& sigEvents = midiNode->timeSignatureChanges();
            const size_t sigCount = std::min(sigSamples.size(), sigEvents.size());
            for (size_t i = 0; i < sigCount; ++i) {
                MasterTrackSnapshot::TimeSignaturePoint point;
                point.timeSeconds = (clipStartSamples + static_cast<double>(sigSamples[i])) / sampleRate;
                point.signature = sigEvents[i];
                snapshot.maxTimeSeconds = std::max(snapshot.maxTimeSeconds, point.timeSeconds);
                snapshot.timeSignaturePoints.push_back(point);
            }
        }
    }

    std::sort(snapshot.tempoPoints.begin(), snapshot.tempoPoints.end(),
        [](const MasterTrackSnapshot::TempoPoint& a, const MasterTrackSnapshot::TempoPoint& b) {
            return a.timeSeconds < b.timeSeconds;
        });

    std::sort(snapshot.timeSignaturePoints.begin(), snapshot.timeSignaturePoints.end(),
        [](const MasterTrackSnapshot::TimeSignaturePoint& a, const MasterTrackSnapshot::TimeSignaturePoint& b) {
            return a.timeSeconds < b.timeSeconds;
        });

    return snapshot;
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

uapmd::AppModel::ProjectResult uapmd::AppModel::saveProject(const std::filesystem::path& projectFile) {
    ProjectResult result;
    if (projectFile.empty()) {
        result.error = "Project path is empty";
        return result;
    }

    try {
        auto projectDir = projectFile.parent_path();
        if (!projectDir.empty())
            std::filesystem::create_directories(projectDir);
        auto clipDir = projectDir / "clips";

        auto project = uapmd::UapmdProjectData::create();
        auto* sequencerEngine = sequencer_.engine();
        auto sequencerTracks = sequencerEngine ? sequencerEngine->tracks() : std::vector<uapmd::SequencerTrack*>{};
        auto timelineTracks = getTimelineTracks();

        size_t midiExportCounter = 0;

        for (size_t trackIndex = 0; trackIndex < timelineTracks.size(); ++trackIndex) {
            auto* timelineTrack = timelineTracks[trackIndex];
            if (!timelineTrack)
                continue;

            auto projectTrack = uapmd::UapmdProjectTrackData::create();
            auto clips = timelineTrack->clipManager().getAllClips();
            std::sort(clips.begin(), clips.end(), [](const uapmd::ClipData* a, const uapmd::ClipData* b) {
                return a->clipId < b->clipId;
            });

            std::unordered_map<int32_t, uapmd::UapmdProjectClipData*> clipLookup;
            clipLookup.reserve(clips.size());

            for (const auto* clip : clips) {
                if (!clip)
                    continue;

                auto projectClip = uapmd::UapmdProjectClipData::create();
                projectClip->clipType(clip->clipType == uapmd::ClipType::Midi ? "midi" : "audio");
                projectClip->tickResolution(clip->tickResolution);
                projectClip->tempo(clip->clipTempo);

                std::filesystem::path clipPath = clip->filepath;
                if (clip->clipType == uapmd::ClipType::Midi) {
                    bool needsExport = clipPath.empty() || !std::filesystem::exists(clipPath);
                    if (needsExport) {
                        std::filesystem::create_directories(clipDir);
                        auto exportName = std::format("track{}_clip_{}_{}.midi2",
                                                      trackIndex,
                                                      clip->clipId,
                                                      midiExportCounter++);
                        auto exportPath = clipDir / exportName;

                        auto* sourceNode = timelineTrack->getSourceNode(clip->sourceNodeInstanceId);
                        auto* midiNode = dynamic_cast<uapmd::MidiClipSourceNode*>(sourceNode);
                        if (!midiNode) {
                            result.error = std::format("Clip {} on track {} is missing MIDI data", clip->clipId, trackIndex);
                            return result;
                        }

                        auto clipUmps = buildSmf2ClipFromMidiNode(*midiNode);
                        if (!uapmd::Smf2ClipReaderWriter::write(exportPath, clipUmps, &result.error))
                            return result;
                        clipPath = exportPath;
                    } else {
                        clipPath = std::filesystem::absolute(clipPath);
                    }
                }

                if (!clipPath.empty() && !projectDir.empty())
                    clipPath = makeRelativePath(projectDir, clipPath);

                projectClip->file(clipPath);

                clipLookup[clip->clipId] = projectClip.get();
                projectTrack->clips().push_back(std::move(projectClip));
            }

            for (const auto* clip : clips) {
                auto it = clipLookup.find(clip->clipId);
                if (it == clipLookup.end())
                    continue;

                uapmd::UapmdTimelinePosition pos{};
                if (clip->anchorClipId >= 0) {
                    auto anchorIt = clipLookup.find(clip->anchorClipId);
                    if (anchorIt != clipLookup.end())
                        pos.anchor = anchorIt->second;
                }
                pos.origin = (clip->anchorOrigin == uapmd::AnchorOrigin::End)
                    ? uapmd::UapmdAnchorOrigin::End
                    : uapmd::UapmdAnchorOrigin::Start;
                pos.samples = static_cast<uint64_t>(std::max<int64_t>(0, clip->anchorOffset.samples));
                it->second->position(pos);
            }

            uapmd::SequencerTrack* sequencerTrack = (sequencerEngine && trackIndex < sequencerTracks.size())
                ? sequencerTracks[trackIndex]
                : nullptr;
            if (auto graphData = createSerializedPluginGraph(sequencerTrack, sequencerEngine))
                projectTrack->graph(std::move(graphData));
            project->addTrack(std::move(projectTrack));
        }

        if (auto* masterTrack = project->masterTrack()) {
            masterTrack->clips().clear();
            auto snapshot = buildMasterTrackSnapshot();
            auto masterClip = uapmd::UapmdProjectClipData::create();

            std::filesystem::path masterFile = clipDir / "master_track.midi2";
            std::filesystem::create_directories(clipDir);
            auto masterUmps = buildMasterTrackSmf2Clip(snapshot);
            if (!uapmd::Smf2ClipReaderWriter::write(masterFile, masterUmps, &result.error))
                return result;

            if (!projectDir.empty())
                masterFile = makeRelativePath(projectDir, masterFile);

            uapmd::UapmdTimelinePosition pos{};
            pos.anchor = nullptr;
            pos.samples = 0;
            masterClip->position(pos);
            masterClip->clipType("midi");
            masterClip->tickResolution(kDefaultDctpq);
            masterClip->tempo(timeline_.tempo);
            masterClip->file(masterFile);
            masterTrack->clips().push_back(std::move(masterClip));

            if (auto graphData = createSerializedPluginGraph(
                    sequencerEngine ? sequencerEngine->masterTrack() : nullptr,
                    sequencerEngine)) {
                masterTrack->graph(std::move(graphData));
            }
        }

        if (!uapmd::UapmdProjectDataWriter::write(project.get(), projectFile)) {
            result.error = "Failed to write project file";
            return result;
        }

        result.success = true;
        return result;
    } catch (const std::exception& e) {
        result.error = e.what();
        return result;
    }
}

uapmd::AppModel::ProjectResult uapmd::AppModel::loadProject(const std::filesystem::path& projectFile) {
    ProjectResult result;
    if (projectFile.empty()) {
        result.error = "Project path is empty";
        return result;
    }

    try {
        auto project = uapmd::UapmdProjectDataReader::read(projectFile);
        if (!project) {
            result.error = "Failed to parse project file";
            return result;
        }

        auto projectDir = projectFile.parent_path();

        timeline_.isPlaying = false;
        timeline_.playheadPosition = uapmd::TimelinePosition{};
        timeline_.loopEnabled = false;
        timeline_.loopStart = uapmd::TimelinePosition{};
        timeline_.loopEnd = uapmd::TimelinePosition{};

        removeAllTracks();

        auto restorePluginsForTrack = [this](uapmd::UapmdProjectTrackData* projectTrack, int32_t trackIndex) {
            if (!projectTrack)
                return;

            auto* graphData = projectTrack->graph();
            if (!graphData)
                return;

            auto plugins = graphData->plugins();
            for (const auto& plugin : plugins) {
                if (plugin.plugin_id.empty() || plugin.format.empty())
                    continue;
                PluginInstanceConfig config{};
                createPluginInstanceAsync(plugin.format, plugin.plugin_id, trackIndex, config);
            }
        };

        auto& tracks = project->tracks();
        std::vector<int32_t> createdTrackIndices;
        createdTrackIndices.reserve(tracks.size());
        for (size_t i = 0; i < tracks.size(); ++i) {
            int32_t trackIndex = addTrack();
            if (trackIndex < 0) {
                result.error = "Failed to create track for project data";
                return result;
            }
            createdTrackIndices.push_back(trackIndex);

            auto* timelineTrack = timeline_tracks_[trackIndex].get();
            if (!timelineTrack)
                continue;

            restorePluginsForTrack(tracks[i], trackIndex);

            auto& projectClips = tracks[i]->clips();
            std::unordered_map<uapmd::UapmdProjectClipData*, int32_t> clipIdMap;
            clipIdMap.reserve(projectClips.size());

            for (auto& clip : projectClips) {
                if (!clip)
                    continue;

                auto absoluteSamples = static_cast<int64_t>(clip->absolutePositionInSamples());
                uapmd::TimelinePosition position;
                position.samples = absoluteSamples;

                int32_t newClipId = -1;
                const auto clipFile = clip->file();
                const auto clipType = clip->clipType();
                std::filesystem::path resolvedPath = clipFile;
                if (!resolvedPath.empty())
                    resolvedPath = makeAbsolutePath(projectDir, resolvedPath);

                if (clipType == "midi") {
                    ParsedSmf2Clip parsed;
                    if (!resolvedPath.empty()) {
                        if (!parseSmf2ClipFile(resolvedPath, parsed, result.error))
                            return result;
                    } else {
                        result.error = "MIDI clip is missing file path";
                        return result;
                    }

                    double clipTempo = clip->tempo();
                    if (clipTempo <= 0.0 && !parsed.tempoChanges.empty())
                        clipTempo = parsed.tempoChanges.front().bpm;
                    if (clipTempo <= 0.0)
                        clipTempo = 120.0;

                    auto loadResult = addMidiClipToTrack(
                        trackIndex,
                        position,
                        std::move(parsed.events),
                        std::move(parsed.eventTicks),
                        parsed.tickResolution,
                        clipTempo,
                        std::move(parsed.tempoChanges),
                        std::move(parsed.timeSignatureChanges),
                        resolvedPath.filename().string());
                    if (!loadResult.success) {
                        result.error = loadResult.error.empty()
                            ? "Failed to load MIDI clip"
                            : loadResult.error;
                        return result;
                    }
                    newClipId = loadResult.clipId;
                } else {
                    auto reader = uapmd::createAudioFileReaderFromPath(resolvedPath);
                    if (!reader) {
                        result.error = std::format("Failed to open audio clip {}", resolvedPath.string());
                        return result;
                    }
                    auto loadResult = addClipToTrack(trackIndex, position, std::move(reader), resolvedPath.string());
                    if (!loadResult.success) {
                        result.error = loadResult.error.empty()
                            ? "Failed to load audio clip"
                            : loadResult.error;
                        return result;
                    }
                    newClipId = loadResult.clipId;
                }

                if (newClipId >= 0)
                    clipIdMap[clip.get()] = newClipId;
            }

            for (auto& clip : projectClips) {
                if (!clip)
                    continue;
                auto itClip = clipIdMap.find(clip.get());
                if (itClip == clipIdMap.end())
                    continue;

                int32_t newClipId = itClip->second;
                int32_t anchorId = -1;
                uapmd::AnchorOrigin anchorOrigin = uapmd::AnchorOrigin::Start;
                const auto position = clip->position();
                if (position.anchor) {
                    if (auto* anchorClip = dynamic_cast<uapmd::UapmdProjectClipData*>(position.anchor)) {
                        auto anchorIt = clipIdMap.find(anchorClip);
                        if (anchorIt != clipIdMap.end())
                            anchorId = anchorIt->second;
                    }
                }
                if (position.origin == uapmd::UapmdAnchorOrigin::End)
                    anchorOrigin = uapmd::AnchorOrigin::End;

                uapmd::TimelinePosition offset = uapmd::TimelinePosition::fromSamples(
                    static_cast<int64_t>(position.samples),
                    sample_rate_);

                timelineTrack->clipManager().setClipAnchor(newClipId, anchorId, anchorOrigin, offset);
            }
        }

        if (tracks.empty()) {
            addTrack();
        }

        if (auto* master = project->masterTrack()) {
            restorePluginsForTrack(master, uapmd::kMasterTrackIndex);
            for (auto& clip : master->clips()) {
                if (!clip)
                    continue;
                auto masterPath = clip->file();
                if (masterPath.empty())
                    continue;
                auto resolved = makeAbsolutePath(projectDir, masterPath);
                ParsedSmf2Clip parsed;
                if (parseSmf2ClipFile(resolved, parsed, result.error)) {
                    if (!parsed.tempoChanges.empty())
                        timeline_.tempo = parsed.tempoChanges.front().bpm;
                    if (!parsed.timeSignatureChanges.empty()) {
                        timeline_.timeSignatureNumerator = parsed.timeSignatureChanges.front().numerator;
                        timeline_.timeSignatureDenominator = parsed.timeSignatureChanges.front().denominator;
                    }
                }
                break;
            }
        }

        result.success = true;
        return result;
    } catch (const std::exception& e) {
        result.error = e.what();
        return result;
    }
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
