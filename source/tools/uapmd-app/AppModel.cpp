
#include <iostream>
#include <chrono>
#include <fstream>
#include <filesystem>
#include <limits>
#include <exception>
#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <map>
#include <unordered_set>
#include <vector>
#include <functional>
#include <cmath>
#include <future>
#include <thread>
#include <string>
#include <sstream>
#include <optional>
#include <format>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#include <choc/text/choc_JSON.h>
#include <choc/audio/choc_AudioFileFormat_WAV.h>
#include <choc/audio/choc_SampleBuffers.h>
#include <umppi/umppi.hpp>
#include "uapmd/uapmd.hpp"
#include "AppModel.hpp"

#define DEFAULT_AUDIO_BUFFER_SIZE 1024
#define DEFAULT_UMP_BUFFER_SIZE 65536
#define DEFAULT_SAMPLE_RATE 48000
#define FIXED_CHANNEL_COUNT 2

std::unique_ptr<uapmd::AppModel> model{};

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

std::string bundleDisplayName(const std::filesystem::path& bundlePath) {
    auto name = bundlePath.filename().string();
    if (!name.empty())
        return name;
    return bundlePath.string();
}

namespace uapmd {

namespace {

std::string outputAlignmentMonitoringPolicyToProjectString(OutputAlignmentMonitoringPolicy policy) {
    switch (policy) {
        case OutputAlignmentMonitoringPolicy::FULLY_COMPENSATED:
            return "fully_compensated";
        case OutputAlignmentMonitoringPolicy::LOW_LATENCY_LIVE_INPUT:
        default:
            return "low_latency_live_input";
    }
}

OutputAlignmentMonitoringPolicy outputAlignmentMonitoringPolicyFromProjectString(const std::string& value) {
    if (value == "fully_compensated")
        return OutputAlignmentMonitoringPolicy::FULLY_COMPENSATED;
    return OutputAlignmentMonitoringPolicy::LOW_LATENCY_LIVE_INPUT;
}

void markTimelineTrackClipsNeedsFileSave(TimelineTrack* track) {
    if (!track)
        return;

    auto clips = track->clipManager().getAllClips();
    for (const auto& clip : clips)
        track->clipManager().setClipNeedsFileSave(clip.clipId, true);
}

void markLoadedArchiveClipsNeedsFileSave(AppModel& appModel) {
    auto timelineTracks = appModel.getTimelineTracks();
    for (auto* track : timelineTracks)
        markTimelineTrackClipsNeedsFileSave(track);
    markTimelineTrackClipsNeedsFileSave(appModel.getMasterTimelineTrack());
}

} // namespace

struct ScopedTempDir {
    explicit ScopedTempDir(std::filesystem::path dir)
        : path(std::move(dir)) {}

    ~ScopedTempDir() {
        if (!path.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    }

    const std::filesystem::path& get() const { return path; }

private:
    std::filesystem::path path;
};

} // namespace uapmd

std::optional<std::filesystem::path> createTempProjectDirectory(std::string& error)
{
    try {
        auto base = std::filesystem::temp_directory_path() / "uapmd";
        std::filesystem::create_directories(base);
        auto seed = std::chrono::steady_clock::now().time_since_epoch().count();
        for (int attempt = 0; attempt < 32; ++attempt) {
            auto candidate = base / std::filesystem::path(std::format("project-{}-{}", seed, attempt));
            std::error_code ec;
            if (std::filesystem::create_directories(candidate, ec))
                return candidate;
            if (ec && ec != std::errc::file_exists) {
                error = ec.message();
                return std::nullopt;
            }
        }
        error = "Unable to allocate a temporary project directory.";
        return std::nullopt;
    } catch (const std::exception& ex) {
        error = ex.what();
        return std::nullopt;
    }
}

class SerializedProjectPluginGraph final : public uapmd::UapmdProjectPluginGraphData {
public:
    std::filesystem::path external_file;
    std::vector<uapmd::UapmdProjectPluginNodeData> nodes;

    std::filesystem::path externalFile() override { return external_file; }
    std::vector<uapmd::UapmdProjectPluginNodeData> plugins() override { return nodes; }
    void externalFile(const std::filesystem::path& f) override { external_file = f; }
    void addPlugin(uapmd::UapmdProjectPluginNodeData node) override { nodes.push_back(std::move(node)); }
    void setPlugins(std::vector<uapmd::UapmdProjectPluginNodeData> newNodes) override { nodes = std::move(newNodes); }
    void clearPlugins() override { nodes.clear(); }
};

struct PendingProjectPluginState {
    int32_t instance_id{-1};
    size_t plugin_order{0};
    uapmd::AudioPluginInstanceAPI* instance{};
    SerializedProjectPluginGraph* graph{};
    size_t node_index{0};
    std::string scope_label;
};

std::unique_ptr<uapmd::UapmdProjectPluginGraphData> createSerializedPluginGraph(
    uapmd::SequencerTrack* sequencerTrack,
    uapmd::SequencerEngine* engine,
    std::vector<PendingProjectPluginState>* pendingStates = nullptr,
    std::string scopeLabel = {})
{
    if (!sequencerTrack)
        return nullptr;

    const auto& orderedIds = sequencerTrack->orderedInstanceIds();
    auto graph = std::make_unique<SerializedProjectPluginGraph>();
    graph->nodes.reserve(orderedIds.size());
    size_t pluginIndex = 0;

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
        nodeData.display_name = instance->displayName();
        if (pendingStates) {
            nodeData.state_file.clear();
        }
        graph->nodes.push_back(std::move(nodeData));
        if (pendingStates) {
            pendingStates->push_back(PendingProjectPluginState{
                .instance_id = instanceId,
                .plugin_order = pluginIndex,
                .instance = instance,
                .graph = graph.get(),
                .node_index = graph->nodes.size() - 1,
                .scope_label = scopeLabel
            });
        }
        ++pluginIndex;
    }

    return graph;
}

std::string writePluginStateBlob(const std::filesystem::path& projectDir,
                                 const std::filesystem::path& pluginStateDir,
                                 const std::string& scopeLabel,
                                 size_t pluginOrder,
                                 int32_t instanceId,
                                 const std::vector<uint8_t>& stateData,
                                 std::string& error) {
    std::error_code createDirEc;
    std::filesystem::create_directories(pluginStateDir, createDirEc);
    if (createDirEc) {
        error = std::format("Failed to create plugin state directory: {} ({})",
                            pluginStateDir.string(),
                            createDirEc.message());
        return {};
    }

    auto filename = std::format("{}_plugin{}_instance{}.state",
                                scopeLabel,
                                pluginOrder,
                                instanceId);
    auto targetPath = pluginStateDir / filename;

    try {
        std::ofstream out(targetPath, std::ios::binary);
        if (!out)
            throw std::runtime_error("Failed to open state file for writing");
        out.write(reinterpret_cast<const char*>(stateData.data()),
                  static_cast<std::streamsize>(stateData.size()));
    } catch (const std::exception& ex) {
        error = std::format("Failed to write plugin state to {}: {}",
                            targetPath.string(),
                            ex.what());
        return {};
    }

    auto recordedPath = targetPath;
    if (!projectDir.empty())
        recordedPath = makeRelativePath(projectDir, recordedPath);
    return recordedPath.generic_string();
}

struct PendingProjectSaveContext {
    std::filesystem::path project_file;
    std::filesystem::path project_dir;
    std::filesystem::path plugin_state_dir;
    std::unique_ptr<uapmd::UapmdProjectData> project;
    std::vector<PendingProjectPluginState> pending_states;
    size_t next_pending_state{0};
    uapmd::AppModel::ProjectSaveCallback callback;
};

GatheredClipEvents gatherMidiClipEvents(const uapmd::MidiClipSourceNode& node, bool includeTimelineMeta) {
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

    if (includeTimelineMeta) {
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
    }

    if (includeTimelineMeta) {
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

std::vector<umppi::Ump> buildSmf2ClipFromMidiNode(const uapmd::MidiClipSourceNode& node, bool includeTimelineMeta) {
    std::vector<umppi::Ump> clip;
    clip.emplace_back(umppi::Ump(umppi::UmpFactory::deltaClockstamp(0)));
    clip.emplace_back(umppi::Ump(umppi::UmpFactory::dctpq(node.tickResolution())));
    clip.emplace_back(umppi::Ump(umppi::UmpFactory::deltaClockstamp(0)));
    clip.push_back(umppi::UmpFactory::startOfClip());

    auto gathered = gatherMidiClipEvents(node, includeTimelineMeta);
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
    auto clipInfo = uapmd::MidiClipReader::readAnyFormat(file);
    if (!clipInfo.success) {
        error = clipInfo.error.empty() ? "Failed to parse SMF2 clip" : clipInfo.error;
        return false;
    }

    parsed.tickResolution = clipInfo.tick_resolution;
    parsed.events = clipInfo.ump_data;
    parsed.eventTicks = clipInfo.ump_tick_timestamps;
    parsed.tempoChanges = clipInfo.tempo_changes;
    parsed.timeSignatureChanges = clipInfo.time_signature_changes;
    return true;
}

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
        pluginScanTool_(remidy_tooling::PluginScanTool::create()),
        transportController_(std::make_unique<TransportController>(this, &sequencer_)),
        sample_rate_(sampleRate),
        audio_buffer_size_(static_cast<uint32_t>(audioBufferSizeInFrames)),
        auto_buffer_size_enabled_(sequencer_.useAutoBufferSize()) {
    sequencer_.engine()->functionBlockManager()->setMidiIOManager(this);

    // Start with a few empty tracks for the DAW layout
    // (Timeline state and preprocess callback are now managed by SequencerEngine)
    constexpr int kInitialTrackCount = 3;
    for (int i = 0; i < kInitialTrackCount; ++i) {
        addTrack();
    }
}

void uapmd::AppModel::notifyUiReady() {
    {
        std::lock_guard<std::mutex> lock(startupScanMutex_);
        uiReady_ = true;
    }
    maybeStartInitialPluginScan();
}

void uapmd::AppModel::notifyPersistentStorageReady() {
    {
        std::lock_guard<std::mutex> lock(startupScanMutex_);
        persistentStorageReady_ = true;
    }
    maybeStartInitialPluginScan();
}

void uapmd::AppModel::maybeStartInitialPluginScan() {
    std::lock_guard<std::mutex> lock(startupScanMutex_);
    if (!uiReady_ || !persistentStorageReady_)
        return;
    if (initialPluginScanStarted_.exchange(true, std::memory_order_acq_rel))
        return;
    performPluginScanning(false, PluginScanRequest::InProcess, 0.0, true);
}

uapmd::AppModel::~AppModel() = default;

uapmd::IDocumentProvider* uapmd::AppModel::documentProvider() {
    if (!documentProvider_) {
        documentProvider_ = createDocumentProvider();
        if (!documentProvider_) {
            std::cerr << "Document provider unavailable; dialogs disabled." << std::endl;
        }
    }
    return documentProvider_.get();
}

void uapmd::AppModel::cancelPluginScanning() {
    if (!isScanning_)
        return;
    scanCancelRequested_.store(true, std::memory_order_release);
}

void uapmd::AppModel::performPluginScanning(bool forceRescan,
                                            PluginScanRequest request,
                                            double remoteTimeoutSeconds,
                                            bool requireFastScanning) {
    if (isScanning_) {
        std::cout << "Plugin scanning already in progress" << std::endl;
        return;
    }

#if defined(__EMSCRIPTEN__)
    if (request == PluginScanRequest::RemoteProcess) {
        for (auto& callback : scanningCompleted) {
            callback(false, "Remote plugin scanning is unavailable on the WebAssembly build.");
        }
        return;
    }
#endif

    isScanning_ = true;
    const char* modeStr = request == PluginScanRequest::RemoteProcess ? "remote process" : "in-process";
    std::cout << "Starting plugin scanning (" << modeStr
              << ", forceRescan: " << forceRescan
              << ", fastOnly: " << requireFastScanning << ")" << std::endl;

    scanCancelRequested_.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(slowScanMutex_);
        slowScanProgress_ = {};
    }

    // Run scanning in a separate thread to avoid blocking the UI
    std::thread scanningThread([this, forceRescan, request, remoteTimeoutSeconds, requireFastScanning]() {
        try {
            bool success = false;
            std::string errorMsg;
            std::string reportText;

            auto& cacheFile = pluginScanTool_->pluginListCacheFile();
            double bundleTimeoutSeconds = (request == PluginScanRequest::RemoteProcess && remoteTimeoutSeconds > 0.0)
                                          ? remoteTimeoutSeconds
                                          : 0.0;
            {
                std::lock_guard<std::mutex> metricsLock(scanMetricsMutex_);
                lastScanBundleDurations_.clear();
            }

            std::unordered_map<std::string, std::chrono::steady_clock::time_point> bundleStartTimes;
            std::unordered_map<std::string, double> bundleDurationsSeconds;
            auto recordBundleStart = [&bundleStartTimes](const std::filesystem::path& bundlePath) {
                bundleStartTimes[bundlePath.string()] = std::chrono::steady_clock::now();
            };
            auto recordBundleEnd = [&bundleStartTimes, &bundleDurationsSeconds](const std::filesystem::path& bundlePath) {
                auto key = bundlePath.string();
                auto it = bundleStartTimes.find(key);
                if (it != bundleStartTimes.end()) {
                    auto elapsed = std::chrono::steady_clock::now() - it->second;
                    bundleDurationsSeconds[key] = std::chrono::duration<double>(elapsed).count();
                    bundleStartTimes.erase(it);
                }
            };

            remidy_tooling::PluginScanObserver observer;
            observer.slowScanStarted = [this](uint32_t totalBundles) {
                std::lock_guard<std::mutex> lock(slowScanMutex_);
                slowScanProgress_.running = true;
                slowScanProgress_.processedBundles = 0;
                slowScanProgress_.totalBundles = totalBundles;
                slowScanProgress_.currentBundle.clear();
            };
            observer.bundleScanStarted = [this, recordBundleStart](const std::filesystem::path& bundlePath) {
                recordBundleStart(bundlePath);
                std::lock_guard<std::mutex> lock(slowScanMutex_);
                slowScanProgress_.running = true;
                slowScanProgress_.currentBundle = bundleDisplayName(bundlePath);
            };
            observer.bundleScanCompleted = [this, recordBundleEnd](const std::filesystem::path& bundlePath) {
                recordBundleEnd(bundlePath);
                std::lock_guard<std::mutex> lock(slowScanMutex_);
                slowScanProgress_.running = true;
                slowScanProgress_.processedBundles += 1;
                slowScanProgress_.currentBundle = bundleDisplayName(bundlePath);
            };
            observer.slowScanCompleted = [this]() {
                std::lock_guard<std::mutex> lock(slowScanMutex_);
                slowScanProgress_.running = false;
                slowScanProgress_.currentBundle.clear();
                if (slowScanProgress_.processedBundles > slowScanProgress_.totalBundles)
                    slowScanProgress_.totalBundles = slowScanProgress_.processedBundles;
            };
            observer.errorOccurred = [&errorMsg](const std::string& message) {
                errorMsg = message;
            };
            observer.shouldCancel = [this]() {
                return scanCancelRequested_.load(std::memory_order_acquire);
            };
            auto mode = (request == PluginScanRequest::RemoteProcess)
                        ? remidy_tooling::ScanMode::Remote
                        : remidy_tooling::ScanMode::InProcess;
            pluginScanTool_->performPluginScanning(requireFastScanning,
                                                   cacheFile,
                                                   mode,
                                                   forceRescan,
                                                   bundleTimeoutSeconds,
                                                   &observer);

            success = errorMsg.empty();
            if (!success) {
                auto scanError = pluginScanTool_->lastScanError();
                errorMsg = scanError.empty()
                               ? "Plugin scanning failed."
                               : scanError;
            }

            {
                std::lock_guard<std::mutex> metricsLock(scanMetricsMutex_);
                lastScanBundleDurations_ = bundleDurationsSeconds;
            }

            if (success) {
                pluginScanTool_->savePluginListCache();
                sequencer_.engine()->pluginHost()->reloadPluginCatalogFromCache();
                reportText = generateScanReport();
            }

            std::cout << "Plugin scanning completed " << (success ? "successfully" : "with errors") << std::endl;

            for (auto& callback : scanningCompleted) {
                callback(success, errorMsg);
            }
            if (success) {
                for (auto& callback : scanReportReady) {
                    callback(reportText);
                }
            }

            isScanning_ = false;
            scanCancelRequested_.store(false, std::memory_order_release);
        } catch (const std::exception& e) {
            std::cout << "Plugin scanning failed with exception: " << e.what() << std::endl;

            // Notify callbacks of failure
            for (auto& callback : scanningCompleted) {
                callback(false, std::string("Exception during scanning: ") + e.what());
            }

            isScanning_ = false;
            scanCancelRequested_.store(false, std::memory_order_release);
        }
    });

    scanningThread.detach();
}

uapmd::AppModel::SlowScanProgressState uapmd::AppModel::slowScanProgress() const {
    std::lock_guard<std::mutex> lock(slowScanMutex_);
    return slowScanProgress_;
}

std::vector<remidy_tooling::BlocklistEntry> uapmd::AppModel::pluginBlocklist() const {
    return pluginScanTool_->blocklistEntries();
}

bool uapmd::AppModel::unblockPluginFromBlocklist(const std::string& entryId) {
    return pluginScanTool_->unblockBundle(entryId);
}

void uapmd::AppModel::clearPluginBlocklist() {
    pluginScanTool_->clearBlocklist();
}

std::string uapmd::AppModel::lastPluginScanError() const {
    return pluginScanTool_->lastScanError();
}

uint8_t uapmd::AppModel::getInstanceGroup(int32_t instanceId) const {
    auto* engine = sequencer_.engine();
    if (!engine)
        return 0xFF;
    return engine->getInstanceGroup(instanceId);
}

bool uapmd::AppModel::setInstanceGroup(int32_t instanceId, uint8_t group) {
    auto* engine = sequencer_.engine();
    if (!engine)
        return false;
    return engine->setInstanceGroup(instanceId, group);
}

void uapmd::AppModel::setAudioEngineEnabled(bool enabled) {
    audioEngineEnabled_.store(enabled, std::memory_order_release);

    auto* host = sequencer_.engine()->pluginHost();
    const bool isPlaying = sequencer_.isAudioPlaying() != 0;
    if (enabled) {
        for (auto id : host->instanceIds())
            host->getInstance(id)->startProcessing();
        sequencer_.engine()->setEngineActive(true);
        if (!isPlaying) {
            if (sequencer_.startAudio() != 0) {
                std::cerr << "Failed to start audio engine" << std::endl;
                audioEngineEnabled_.store(false, std::memory_order_release);
                sequencer_.engine()->setEngineActive(false);
            }
        }
    } else if (isPlaying) {
        transportController_->stop();
        // Silence the output first, then let the audio callback run for a couple
        // of cycles so the hardware ring buffer drains with silence before we stop.
        sequencer_.engine()->setEngineActive(false);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        sequencer_.stopAudio();
        for (auto id : host->instanceIds())
            host->getInstance(id)->stopProcessing();
    }
}

void uapmd::AppModel::toggleAudioEngine() {
    bool desired = !audioEngineEnabled_.load(std::memory_order_acquire);
    setAudioEngineEnabled(desired);
}

void uapmd::AppModel::updateAudioDeviceSettings(int32_t sampleRate, uint32_t bufferSize) {
    // These UI settings are forwarded to RealtimeSequencer -> DeviceIODispatcher,
    // which ultimately sets OboeAudioIODevice::preferred_callback_frames_.  That
    // value is the block size the sequencer renders for every plugin regardless
    // of the device's framesPerBurst, so edits here change the host buffer size.
    if (sampleRate > 0)
        sample_rate_ = sampleRate;
    if (bufferSize > 0)
        audio_buffer_size_ = bufferSize;
}

void uapmd::AppModel::setAutoBufferSizeEnabled(bool enabled) {
    sequencer_.setUseAutoBufferSize(enabled);
    auto_buffer_size_enabled_ = sequencer_.useAutoBufferSize();
}

void uapmd::AppModel::createPluginInstanceAsync(const std::string& format,
                                                 const std::string& pluginId,
                                                 int32_t trackIndex,
                                                 const PluginInstanceConfig& config,
                                                 std::function<void(const PluginInstanceResult&)> completionCallback) {
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

    // This is the same logic as VirtualMidiDeviceController::createDevice
    // but we call the callback instead of managing state
    std::string formatCopy = format;
    std::string pluginIdCopy = pluginId;

    auto instantiateCallback = [this, config, pluginName, completionCallback](int32_t instanceId, int32_t trackIndex, std::string error) {
        PluginInstanceResult result;
        result.instanceId = instanceId;
        result.pluginName = pluginName;
        result.error = std::move(error);

        if (!result.error.empty() || instanceId < 0) {
            if (completionCallback) {
                completionCallback(result);
            }
            for (auto& cb : instanceCreated) {
                cb(result);
            }
            return;
        }

        std::optional<PluginInstanceConfig> configOverride{config};
        result = registerPluginInstanceInternal(instanceId, configOverride);

        if (completionCallback) {
            completionCallback(result);
        }

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

uapmd::AppModel::PluginInstanceResult uapmd::AppModel::registerPluginInstanceInternal(
    int32_t instanceId,
    const std::optional<PluginInstanceConfig>& configOverride) {
    PluginInstanceResult result;
    result.instanceId = instanceId;

    auto* instance = sequencer_.engine()->getPluginInstance(instanceId);
    if (!instance) {
        result.error = "Plugin instance not found";
        return result;
    }

    std::string pluginName = instance->displayName();
    std::string pluginFormat = instance->formatName();
    std::string pluginIdentifier = instance->pluginId();

    result.pluginName = pluginName;

    PluginInstanceConfig config = configOverride.value_or(PluginInstanceConfig{});
    if (config.apiName.empty()) {
        config.apiName = "default";
    }
    if (config.manufacturer.empty()) {
        config.manufacturer = "UAPMD Project";
    }
    if (config.version.empty()) {
        config.version = "0.1";
    }

    std::string deviceLabel = config.deviceName.empty()
        ? std::format("{} [{}]", pluginName, pluginFormat)
        : config.deviceName;

    auto state = std::make_shared<DeviceState>();
    state->label = deviceLabel;
    state->apiName = config.apiName;
    state->instantiating = false;

    auto& pluginNode = state->pluginInstances[instanceId];
    pluginNode.instanceId = instanceId;
    pluginNode.pluginName = pluginName;
    pluginNode.pluginFormat = pluginFormat;
    pluginNode.pluginId = pluginIdentifier;
    pluginNode.statusMessage = std::format("Plugin ready (instance {})", instanceId);
    pluginNode.instantiating = false;
    pluginNode.hasError = false;

    {
        std::lock_guard lock(devicesMutex_);
        for (auto it = devices_.begin(); it != devices_.end();) {
            auto existingState = it->state;
            if (existingState && existingState->pluginInstances.count(instanceId) > 0) {
                it = devices_.erase(it);
            } else {
                ++it;
            }
        }
        devices_.push_back(DeviceEntry{nextDeviceId_++, state});
    }

    if (configOverride && !configOverride->stateFile.empty()) {
        auto stateResult = loadPluginStateSync(instanceId, configOverride->stateFile.string());
        if (!stateResult.success) {
            std::cerr << "Automatic plugin state load failed for " << pluginName
                      << ": " << stateResult.error << std::endl;
        }
    }

    if (midiApiSupportsDynamicUmpEndpoints(config.apiName)) {
        enableUmpDevice(instanceId, deviceLabel);
    } else {
        state->running = false;
        state->hasError = true;
        state->statusMessage = "Dynamic Virtual MIDI 2.0 devices are unavailable on this platform.";
    }

    result.device = state->device;
    return result;
}

void uapmd::AppModel::clearDeviceEntries() {
    std::lock_guard lock(devicesMutex_);
    devices_.clear();
    nextDeviceId_ = 1;
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

    if (!midiApiSupportsDynamicUmpEndpoints(deviceState->apiName)) {
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
    if (!appModel_->isAudioEngineEnabled())
        return;
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
    if (!appModel_->isAudioEngineEnabled())
        return;
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

void uapmd::AppModel::loadPluginState(int32_t instanceId, const std::string& filepath, PluginStateCallback callback) {
    PluginStateResult result;
    result.instanceId = instanceId;
    result.filepath = filepath;

    auto* instance = sequencer_.engine()->getPluginInstance(instanceId);
    if (!instance) {
        result.success = false;
        result.error = "Failed to get plugin instance";
        std::cerr << result.error << std::endl;
        if (callback)
            callback(std::move(result));
        return;
    }

    std::vector<uint8_t> stateData;
    try {
        std::ifstream file(filepath, std::ios::binary | std::ios::ate);
        if (!file.is_open())
            throw std::runtime_error("Failed to open file for reading");

        auto fileSize = file.tellg();
        file.seekg(0, std::ios::beg);

        stateData.resize(static_cast<size_t>(fileSize));
        file.read(reinterpret_cast<char*>(stateData.data()), fileSize);
        file.close();
    } catch (const std::exception& ex) {
        result.success = false;
        result.error = std::format("Failed to load plugin state: {}", ex.what());
        std::cerr << result.error << std::endl;
        if (callback)
            callback(std::move(result));
        return;
    }

    instance->loadState(std::move(stateData), uapmd::StateContextType::Project, false, nullptr,
                        [callback = std::move(callback), result](std::string error, void* callbackContext) mutable {
                            auto completed = result;
                            if (!error.empty()) {
                                completed.success = false;
                                completed.error = std::move(error);
                                std::cerr << completed.error << std::endl;
                            } else {
                                completed.success = true;
                                std::cout << "Plugin state loaded from: " << completed.filepath << std::endl;
                            }
                            if (callback)
                                callback(std::move(completed));
                        });
}

void uapmd::AppModel::loadPluginState(int32_t instanceId, DocumentHandle handle, PluginStateCallback callback) {
    PluginStateResult result;
    result.instanceId = instanceId;
    result.filepath = handle.display_name.empty() ? handle.id : handle.display_name;

    auto* instance = sequencer_.engine()->getPluginInstance(instanceId);
    if (!instance) {
        result.success = false;
        result.error = "Failed to get plugin instance";
        std::cerr << result.error << std::endl;
        if (callback)
            callback(std::move(result));
        return;
    }

    auto* provider = documentProvider();
    if (!provider) {
        result.success = false;
        result.error = "Document provider unavailable";
        std::cerr << result.error << std::endl;
        if (callback)
            callback(std::move(result));
        return;
    }

    provider->readDocument(std::move(handle),
                           [instance, callback = std::move(callback), result](DocumentIOResult ioResult, std::vector<uint8_t> data) mutable {
                               auto completed = result;
                               if (!ioResult.success) {
                                   completed.success = false;
                                   completed.error = ioResult.error;
                                   std::cerr << completed.error << std::endl;
                                   if (callback)
                                       callback(std::move(completed));
                                   return;
                               }

                               instance->loadState(std::move(data), uapmd::StateContextType::Project, false, nullptr,
                                                   [callback = std::move(callback), completed](std::string error, void* callbackContext) mutable {
                                                       auto finalResult = completed;
                                                       if (!error.empty()) {
                                                           finalResult.success = false;
                                                           finalResult.error = std::move(error);
                                                           std::cerr << finalResult.error << std::endl;
                                                       } else {
                                                           finalResult.success = true;
                                                           std::cout << "Plugin state loaded from: " << finalResult.filepath << std::endl;
                                                       }
                                                       if (callback)
                                                           callback(std::move(finalResult));
                                                   });
                           });
}

uapmd::AppModel::PluginStateResult uapmd::AppModel::loadPluginStateSync(int32_t instanceId, const std::string& filepath) {
    auto promise = std::make_shared<std::promise<PluginStateResult>>();
    auto future = promise->get_future();
    loadPluginState(instanceId, filepath,
                    [promise](PluginStateResult result) {
                        promise->set_value(std::move(result));
                    });
    return future.get();
}

void uapmd::AppModel::savePluginState(int32_t instanceId, const std::string& filepath, PluginStateCallback callback) {
    PluginStateResult result;
    result.instanceId = instanceId;
    result.filepath = filepath;

    auto* instance = sequencer_.engine()->getPluginInstance(instanceId);
    if (!instance) {
        result.success = false;
        result.error = "Failed to get plugin instance";
        std::cerr << result.error << std::endl;
        if (callback)
            callback(std::move(result));
        return;
    }

    instance->requestState(uapmd::StateContextType::Project, false, nullptr,
                           [callback = std::move(callback), result, filepath](std::vector<uint8_t> state, std::string error, void* callbackContext) mutable {
                               auto completed = result;
                               if (!error.empty()) {
                                   completed.success = false;
                                   completed.error = std::move(error);
                                   std::cerr << completed.error << std::endl;
                                   if (callback)
                                       callback(std::move(completed));
                                   return;
                               }

                               try {
                                   std::ofstream file(filepath, std::ios::binary);
                                   if (!file.is_open())
                                       throw std::runtime_error("Failed to open file for writing");
                                   file.write(reinterpret_cast<const char*>(state.data()), static_cast<std::streamsize>(state.size()));
                                   file.close();
                                   completed.success = true;
                                   std::cout << "Plugin state saved to: " << filepath << std::endl;
                               } catch (const std::exception& ex) {
                                   completed.success = false;
                                   completed.error = std::format("Failed to save plugin state: {}", ex.what());
                                   std::cerr << completed.error << std::endl;
                               }

                               if (callback)
                                   callback(std::move(completed));
                           });
}

void uapmd::AppModel::savePluginState(int32_t instanceId, DocumentHandle handle, PluginStateCallback callback) {
    PluginStateResult result;
    result.instanceId = instanceId;
    result.filepath = handle.display_name.empty() ? handle.id : handle.display_name;

    auto* instance = sequencer_.engine()->getPluginInstance(instanceId);
    if (!instance) {
        result.success = false;
        result.error = "Failed to get plugin instance";
        std::cerr << result.error << std::endl;
        if (callback)
            callback(std::move(result));
        return;
    }

    if (!documentProvider()) {
        result.success = false;
        result.error = "Document provider unavailable";
        std::cerr << result.error << std::endl;
        if (callback)
            callback(std::move(result));
        return;
    }

    instance->requestState(uapmd::StateContextType::Project, false, nullptr,
                           [handle = std::move(handle), callback = std::move(callback), result](std::vector<uint8_t> state, std::string error, void* callbackContext) mutable {
                               auto completed = result;
                               if (!error.empty()) {
                                   completed.success = false;
                                   completed.error = std::move(error);
                                   std::cerr << completed.error << std::endl;
                                   if (callback)
                                       callback(std::move(completed));
                                   return;
                               }

                               auto* provider = uapmd::AppModel::instance().documentProvider();
                               if (!provider) {
                                   completed.success = false;
                                   completed.error = "Document provider unavailable";
                                   std::cerr << completed.error << std::endl;
                                   if (callback)
                                       callback(std::move(completed));
                                   return;
                               }

                               provider->writeDocument(std::move(handle), std::move(state),
                                                       [callback = std::move(callback), completed](DocumentIOResult ioResult) mutable {
                                                           auto finalResult = completed;
                                                           finalResult.success = ioResult.success;
                                                           finalResult.error = ioResult.error;
                                                           if (finalResult.success) {
                                                               std::cout << "Plugin state saved to: " << finalResult.filepath << std::endl;
                                                           } else {
                                                               std::cerr << finalResult.error << std::endl;
                                                           }
                                                           if (callback)
                                                               callback(std::move(finalResult));
                                                       });
                           });
}

uapmd::AppModel::PluginStateResult uapmd::AppModel::savePluginStateSync(int32_t instanceId, const std::string& filepath) {
    auto promise = std::make_shared<std::promise<PluginStateResult>>();
    auto future = promise->get_future();
    savePluginState(instanceId, filepath,
                    [promise](PluginStateResult result) {
                        promise->set_value(std::move(result));
                    });
    return future.get();
}

// Timeline and clip management

uapmd::AppModel::ClipAddResult uapmd::AppModel::addClipToTrack(
    int32_t trackIndex,
    const uapmd::TimelinePosition& position,
    std::unique_ptr<uapmd::AudioFileReader> reader,
    const std::string& filepath
) {
    ClipAddResult result;

    // Detect MIDI files by extension and route to addMidiClipToTrack
    if (!filepath.empty()) {
        std::filesystem::path path(filepath);
        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".mid" || ext == ".midi" || ext == ".smf" || ext == ".midi2")
            return addMidiClipToTrack(trackIndex, position, filepath);
    }

    auto engineResult = sequencer_.engine()->timeline().addAudioClipToTrack(trackIndex, position, std::move(reader), filepath);
    result.clipId = engineResult.clipId;
    result.sourceNodeId = engineResult.sourceNodeId;
    result.success = engineResult.success;
    result.error = engineResult.error;
    return result;
}

uapmd::AppModel::ClipAddResult uapmd::AppModel::addMidiClipToTrack(
    int32_t trackIndex,
    const uapmd::TimelinePosition& position,
    const std::string& filepath
) {
    ClipAddResult result;
    auto clipInfo = uapmd::MidiClipReader::readAnyFormat(filepath);
    if (!clipInfo.success) {
        result.error = clipInfo.error;
        return result;
    }

    auto separated = uapmd::MidiClipReader::separateMasterTrackEvents(std::move(clipInfo));
    auto& musicalClip = separated.musicalClip;
    auto clipTempo = musicalClip.tempo_changes.empty() ? musicalClip.tempo : musicalClip.tempo_changes.front().bpm;
    if (clipTempo <= 0.0)
        clipTempo = 120.0;

    if (separated.hasMusicalClip()) {
        auto engineResult = sequencer_.engine()->timeline().addMidiClipToTrack(
            trackIndex, position,
            std::move(musicalClip.ump_data),
            std::move(musicalClip.ump_tick_timestamps),
            musicalClip.tick_resolution,
            clipTempo,
            std::move(musicalClip.tempo_changes),
            std::move(musicalClip.time_signature_changes),
            std::filesystem::path(filepath).stem().string(),
            false,
            separated.hasMasterTrackClip());
        result.clipId = engineResult.clipId;
        result.sourceNodeId = engineResult.sourceNodeId;
        result.success = engineResult.success;
        result.error = engineResult.error;
        if (!result.success)
            return result;
    } else {
        result.success = true;
    }

    if (separated.hasMasterTrackClip()) {
        auto& masterClip = separated.masterTrackClip;
        auto masterResult = sequencer_.engine()->timeline().addMasterMidiClip(
            position,
            {},
            {},
            masterClip.tick_resolution,
            masterClip.tempo,
            std::move(masterClip.tempo_changes),
            std::move(masterClip.time_signature_changes),
            std::format("{} Meta", std::filesystem::path(filepath).stem().string()));
        if (!masterResult.success) {
            result.success = false;
            result.error = masterResult.error;
        }
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
    const std::string& clipName,
    bool needsFileSave
) {
    ClipAddResult result;
    auto engineResult = sequencer_.engine()->timeline().addMidiClipToTrack(
        trackIndex, position,
        std::move(umpEvents), std::move(umpTickTimestamps),
        tickResolution, clipTempo,
        std::move(tempoChanges), std::move(timeSignatureChanges),
        clipName,
        false,
        needsFileSave);
    result.clipId = engineResult.clipId;
    result.sourceNodeId = engineResult.sourceNodeId;
    result.success = engineResult.success;
    result.error = engineResult.error;

    return result;
}

uapmd::AppModel::ClipAddResult uapmd::AppModel::addMasterMidiClip(
    const uapmd::TimelinePosition& position,
    std::vector<uapmd_ump_t> umpEvents,
    std::vector<uint64_t> umpTickTimestamps,
    uint32_t tickResolution,
    double clipTempo,
    std::vector<MidiTempoChange> tempoChanges,
    std::vector<MidiTimeSignatureChange> timeSignatureChanges,
    const std::string& clipName,
    bool needsFileSave,
    const std::string& filepath
) {
    ClipAddResult result;
    auto engineResult = sequencer_.engine()->timeline().addMasterMidiClip(
        position,
        std::move(umpEvents), std::move(umpTickTimestamps),
        tickResolution, clipTempo,
        std::move(tempoChanges), std::move(timeSignatureChanges),
        clipName,
        needsFileSave,
        filepath);
    result.clipId = engineResult.clipId;
    result.sourceNodeId = engineResult.sourceNodeId;
    result.success = engineResult.success;
    result.error = engineResult.error;
    return result;
}

bool uapmd::AppModel::removeClipFromTrack(int32_t trackIndex, int32_t clipId) {
    return sequencer_.engine()->timeline().removeClipFromTrack(trackIndex, clipId);
}

// ── UMP-level clip editing ────────────────────────────────────────────────────

// Internal helper: look up a MIDI source node, run modifier(words, ticks, error),
// then commit by replacing the clip source node.
namespace {

constexpr std::string_view kMasterMarkerReferenceId = "master_track";

int64_t secondsToSamples(double seconds, double sampleRate) {
    return static_cast<int64_t>(std::llround(seconds * sampleRate));
}

bool referencesThisClipEnd(const uapmd::ClipMarker& marker, std::string_view clipReferenceId) {
    const auto reference = marker.timeReference(clipReferenceId, kMasterMarkerReferenceId);
    return reference.type == uapmd::TimeReferenceType::ContainerEnd &&
           (reference.referenceId.empty() || reference.referenceId == clipReferenceId);
}

bool referencesThisClipEnd(const uapmd::AudioWarpPoint& warp, std::string_view clipReferenceId) {
    const auto reference = warp.timeReference(clipReferenceId, kMasterMarkerReferenceId);
    return reference.type == uapmd::TimeReferenceType::ContainerEnd &&
           (reference.referenceId.empty() || reference.referenceId == clipReferenceId);
}

std::unordered_map<std::string, uapmd::ClipData> buildClipReferenceMap(const std::vector<uapmd::TimelineTrack*>& tracks) {
    std::unordered_map<std::string, uapmd::ClipData> clipLookup;
    for (auto* track : tracks) {
        if (!track)
            continue;
        for (auto& clip : track->clipManager().getAllClips())
            clipLookup[clip.referenceId] = std::move(clip);
    }
    return clipLookup;
}

const uapmd::ClipMarker* findMarkerById(const std::vector<uapmd::ClipMarker>& markers, std::string_view markerId) {
    auto it = std::find_if(markers.begin(), markers.end(), [markerId](const auto& marker) {
        return marker.markerId == markerId;
    });
    return it == markers.end() ? nullptr : &(*it);
}

struct MarkerKey {
    std::string clipReferenceId;
    std::string markerId;

    bool operator==(const MarkerKey& other) const {
        return clipReferenceId == other.clipReferenceId && markerId == other.markerId;
    }
};

struct MarkerKeyHash {
    size_t operator()(const MarkerKey& key) const {
        size_t seed = std::hash<std::string>{}(key.clipReferenceId);
        seed ^= std::hash<std::string>{}(key.markerId) + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
        return seed;
    }
};

std::optional<int64_t> resolveMarkerAbsoluteSample(
    std::string_view ownerReferenceId,
    const uapmd::ClipMarker& marker,
    const std::unordered_map<std::string, uapmd::ClipData>& clipLookup,
    const std::vector<uapmd::ClipMarker>& masterTrackMarkers,
    std::unordered_map<MarkerKey, std::optional<int64_t>, MarkerKeyHash>& cache,
    std::unordered_set<MarkerKey, MarkerKeyHash>& resolving
);

std::optional<int64_t> resolveReferenceAbsoluteSample(
    std::string_view ownerReferenceId,
    const uapmd::ClipData* ownerClip,
    const uapmd::TimeReference& reference,
    const std::unordered_map<std::string, uapmd::ClipData>& clipLookup,
    const std::vector<uapmd::ClipMarker>& masterTrackMarkers,
    std::unordered_map<MarkerKey, std::optional<int64_t>, MarkerKeyHash>& cache,
    std::unordered_set<MarkerKey, MarkerKeyHash>& resolving
) {
    switch (reference.type) {
        case uapmd::TimeReferenceType::ContainerStart: {
            const std::string effectiveReferenceId = reference.referenceId.empty()
                ? std::string(ownerReferenceId)
                : reference.referenceId;
            if (effectiveReferenceId == kMasterMarkerReferenceId)
                return 0;
            if (effectiveReferenceId == ownerReferenceId)
                return ownerClip ? std::optional<int64_t>(ownerClip->position.samples) : std::optional<int64_t>(0);
            auto clipIt = clipLookup.find(effectiveReferenceId);
            return clipIt == clipLookup.end() ? std::nullopt : std::optional<int64_t>(clipIt->second.position.samples);
        }
        case uapmd::TimeReferenceType::ContainerEnd: {
            const std::string effectiveReferenceId = reference.referenceId.empty()
                ? std::string(ownerReferenceId)
                : reference.referenceId;
            if (effectiveReferenceId == kMasterMarkerReferenceId)
                return std::nullopt;
            if (effectiveReferenceId == ownerReferenceId) {
                if (!ownerClip)
                    return std::nullopt;
                return ownerClip->position.samples + ownerClip->durationSamples;
            }
            auto clipIt = clipLookup.find(effectiveReferenceId);
            if (clipIt == clipLookup.end())
                return std::nullopt;
            return clipIt->second.position.samples + clipIt->second.durationSamples;
        }
        case uapmd::TimeReferenceType::Point: {
            std::string containerReferenceId;
            std::string pointReferenceId;
            if (!uapmd::TimeReference::parsePointReferenceId(reference.referenceId, containerReferenceId, pointReferenceId))
                return std::nullopt;
            if (containerReferenceId == kMasterMarkerReferenceId) {
                auto* marker = findMarkerById(masterTrackMarkers, pointReferenceId);
                if (!marker)
                    return std::nullopt;
                return resolveMarkerAbsoluteSample(kMasterMarkerReferenceId, *marker, clipLookup, masterTrackMarkers, cache, resolving);
            }
            auto clipIt = clipLookup.find(containerReferenceId);
            if (clipIt == clipLookup.end())
                return std::nullopt;
            auto* marker = findMarkerById(clipIt->second.markers, pointReferenceId);
            if (!marker)
                return std::nullopt;
            return resolveMarkerAbsoluteSample(containerReferenceId, *marker, clipLookup, masterTrackMarkers, cache, resolving);
        }
    }
    return std::nullopt;
}

std::optional<int64_t> resolveMarkerAbsoluteSample(
    std::string_view ownerReferenceId,
    const uapmd::ClipMarker& marker,
    const std::unordered_map<std::string, uapmd::ClipData>& clipLookup,
    const std::vector<uapmd::ClipMarker>& masterTrackMarkers,
    std::unordered_map<MarkerKey, std::optional<int64_t>, MarkerKeyHash>& cache,
    std::unordered_set<MarkerKey, MarkerKeyHash>& resolving
) {
    MarkerKey key{std::string(ownerReferenceId), marker.markerId};
    if (auto it = cache.find(key); it != cache.end())
        return it->second;
    if (!resolving.insert(key).second) {
        cache[key] = std::nullopt;
        return std::nullopt;
    }

    const uapmd::ClipData* ownerClip = nullptr;
    if (ownerReferenceId != kMasterMarkerReferenceId) {
        auto clipIt = clipLookup.find(std::string(ownerReferenceId));
        if (clipIt != clipLookup.end())
            ownerClip = &clipIt->second;
    }

    auto absoluteReferenceSample = resolveReferenceAbsoluteSample(
        ownerReferenceId,
        ownerClip,
        marker.timeReference(ownerReferenceId, kMasterMarkerReferenceId),
        clipLookup,
        masterTrackMarkers,
        cache,
        resolving);

    std::optional<int64_t> resolved;
    if (absoluteReferenceSample) {
        const double sampleRate = std::max(1.0, static_cast<double>(uapmd::AppModel::instance().sampleRate()));
        resolved = *absoluteReferenceSample + secondsToSamples(marker.clipPositionOffset, sampleRate);
    }

    resolving.erase(key);
    cache[key] = resolved;
    return resolved;
}

std::optional<int64_t> resolveAudioWarpClipPosition(
    const uapmd::ClipData& targetClip,
    const uapmd::AudioWarpPoint& warp,
    const std::unordered_map<std::string, uapmd::ClipData>& clipLookup,
    const std::vector<uapmd::ClipMarker>& masterTrackMarkers
) {
    std::unordered_map<MarkerKey, std::optional<int64_t>, MarkerKeyHash> cache;
    std::unordered_set<MarkerKey, MarkerKeyHash> resolving;
    auto absoluteReferenceSample = resolveReferenceAbsoluteSample(
        targetClip.referenceId,
        &targetClip,
        warp.timeReference(targetClip.referenceId, kMasterMarkerReferenceId),
        clipLookup,
        masterTrackMarkers,
        cache,
        resolving);
    if (!absoluteReferenceSample)
        return std::nullopt;
    const double sampleRate = std::max(1.0, static_cast<double>(uapmd::AppModel::instance().sampleRate()));
    const int64_t absoluteSample = *absoluteReferenceSample + secondsToSamples(warp.clipPositionOffset, sampleRate);
    const int64_t clipPosition = absoluteSample - targetClip.position.samples;
    if (clipPosition < 0 || clipPosition > targetClip.durationSamples)
        return std::nullopt;
    return clipPosition;
}

std::vector<uapmd::AudioWarpPoint> resolveAudioWarpPoints(
    const uapmd::ClipData& targetClip,
    const std::vector<uapmd::AudioWarpPoint>& audioWarps,
    const std::unordered_map<std::string, uapmd::ClipData>& clipLookup,
    const std::vector<uapmd::ClipMarker>& masterTrackMarkers
) {
    const double sampleRate = std::max(1.0, static_cast<double>(uapmd::AppModel::instance().sampleRate()));
    std::vector<uapmd::AudioWarpPoint> resolved;
    resolved.reserve(audioWarps.size());
    for (auto warp : audioWarps) {
        if (auto clipPosition = resolveAudioWarpClipPosition(targetClip, warp, clipLookup, masterTrackMarkers)) {
            warp.clipPositionOffset = static_cast<double>(*clipPosition) / sampleRate;
            resolved.push_back(std::move(warp));
        }
    }
    return resolved;
}

bool validateMarkerGraphAcyclic(
    std::string_view ownerReferenceId,
    const std::vector<uapmd::ClipMarker>& markers,
    const std::unordered_map<std::string, uapmd::ClipData>& clipLookup,
    const std::vector<uapmd::ClipMarker>& masterTrackMarkers
) {
    std::unordered_map<std::string, std::vector<uapmd::ClipMarker>> markersByOwner;
    for (const auto& [referenceId, clipData] : clipLookup)
        markersByOwner[referenceId] = clipData.markers;
    markersByOwner[std::string(ownerReferenceId)] = markers;
    markersByOwner[std::string(kMasterMarkerReferenceId)] = masterTrackMarkers;

    auto findMarker = [&](const MarkerKey& key) -> const uapmd::ClipMarker* {
        auto ownerIt = markersByOwner.find(key.clipReferenceId);
        if (ownerIt == markersByOwner.end())
            return nullptr;
        return findMarkerById(ownerIt->second, key.markerId);
    };

    for (const auto& marker : markers) {
        MarkerKey start{std::string(ownerReferenceId), marker.markerId};
        std::unordered_set<MarkerKey, MarkerKeyHash> visited;
        std::function<bool(const MarkerKey&)> visit = [&](const MarkerKey& key) -> bool {
            if (!visited.insert(key).second)
                return true;
            const auto* current = findMarker(key);
            if (!current)
                return false;
            const auto reference = current->timeReference(key.clipReferenceId, kMasterMarkerReferenceId);
            if (reference.type != uapmd::TimeReferenceType::Point)
                return false;

            std::string nextOwner;
            std::string nextMarker;
            if (!uapmd::TimeReference::parsePointReferenceId(reference.referenceId, nextOwner, nextMarker))
                return false;
            if (nextMarker.empty())
                return false;
            return visit(MarkerKey{std::move(nextOwner), std::move(nextMarker)});
        };

        if (visit(start))
            return false;
    }
    return true;
}

void resolveAllClipAnchorsInAppModel(uapmd::AppModel& appModel) {
    auto tracks = appModel.getTimelineTracks();

    struct ClipRecord {
        uapmd::ClipManager* clipManager{nullptr};
        uapmd::ClipData clip;
    };

    std::unordered_map<std::string, ClipRecord> clipRecords;
    auto collectTrack = [&clipRecords](uapmd::TimelineTrack* track) {
        if (!track)
            return;
        for (const auto& clip : track->clipManager().getAllClips())
            clipRecords.emplace(clip.referenceId, ClipRecord{&track->clipManager(), clip});
    };

    for (auto* track : tracks)
        collectTrack(track);
    collectTrack(appModel.getMasterTimelineTrack());

    std::unordered_map<std::string, uapmd::TimelinePosition> resolvedPositions;
    std::unordered_set<std::string> resolving;

    std::function<uapmd::TimelinePosition(const std::string&)> resolveClipPosition =
        [&](const std::string& key) -> uapmd::TimelinePosition {
            if (auto it = resolvedPositions.find(key); it != resolvedPositions.end())
                return it->second;

            auto recordIt = clipRecords.find(key);
            if (recordIt == clipRecords.end())
                return {};

            const auto& clip = recordIt->second.clip;
            const auto timeReference = clip.timeReference(appModel.sampleRate());
            if (timeReference.referenceId.empty()) {
                auto resolved = uapmd::TimelinePosition::fromSeconds(timeReference.offset, appModel.sampleRate());
                resolvedPositions[key] = resolved;
                return resolved;
            }

            if (!resolving.insert(key).second) {
                auto resolved = uapmd::TimelinePosition::fromSeconds(timeReference.offset, appModel.sampleRate());
                resolvedPositions[key] = resolved;
                return resolved;
            }

            auto anchorIt = clipRecords.find(timeReference.referenceId);
            if (anchorIt == clipRecords.end()) {
                resolving.erase(key);
                auto resolved = uapmd::TimelinePosition::fromSeconds(timeReference.offset, appModel.sampleRate());
                resolvedPositions[key] = resolved;
                return resolved;
            }

            auto anchorPosition = resolveClipPosition(timeReference.referenceId);
            if (timeReference.type == uapmd::TimeReferenceType::ContainerEnd)
                anchorPosition.samples += anchorIt->second.clip.durationSamples;

            auto resolved = anchorPosition + uapmd::TimelinePosition::fromSeconds(timeReference.offset, appModel.sampleRate());
            resolvedPositions[key] = resolved;
            resolving.erase(key);
            return resolved;
        };

    for (const auto& [key, record] : clipRecords)
        record.clipManager->setClipPosition(record.clip.clipId, resolveClipPosition(key));
}

} // namespace

static bool modifyMidiClipUmp(
    int32_t trackIndex, int32_t clipId,
    const std::function<bool(std::vector<uapmd_ump_t>&,
                             std::vector<uint64_t>&,
                             std::string&)>& modifier,
    std::string& error)
{
    auto& appModel = uapmd::AppModel::instance();
    auto tracks = appModel.getTimelineTracks();
    if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(tracks.size()) || !tracks[trackIndex]) {
        error = "Track not found";
        return false;
    }
    auto* clip = tracks[trackIndex]->clipManager().getClip(clipId);
    if (!clip) { error = "Clip not found"; return false; }
    auto sourceNode = tracks[trackIndex]->getSourceNode(clip->sourceNodeInstanceId);
    auto midiNode   = std::dynamic_pointer_cast<uapmd::MidiClipSourceNode>(sourceNode);
    if (!midiNode)  { error = "Not a MIDI clip"; return false; }

    auto newWords = midiNode->umpEvents();
    auto newTicks = midiNode->eventTimestampsTicks();

    if (!modifier(newWords, newTicks, error))
        return false;

    auto newNode = std::make_unique<uapmd::MidiClipSourceNode>(
        midiNode->instanceId(),
        std::move(newWords),
        std::move(newTicks),
        midiNode->tickResolution(),
        midiNode->clipTempo(),
        static_cast<double>(appModel.sampleRate()),
        midiNode->tempoChanges(),
        midiNode->timeSignatureChanges());

    if (!tracks[trackIndex]->replaceClipSourceNode(clipId, std::move(newNode))) {
        error = "Failed to replace clip data";
        return false;
    }
    return true;
}

choc::value::Value uapmd::AppModel::getMidiClipUmpEvents(int32_t trackIndex, int32_t clipId)
{
    auto tracks = getTimelineTracks();
    if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(tracks.size()) || !tracks[trackIndex])
        throw std::invalid_argument("Track not found");
    auto* clip = tracks[trackIndex]->clipManager().getClip(clipId);
    if (!clip) throw std::invalid_argument("Clip not found");
    auto sourceNode = tracks[trackIndex]->getSourceNode(clip->sourceNodeInstanceId);
    auto midiNode   = std::dynamic_pointer_cast<uapmd::MidiClipSourceNode>(sourceNode);
    if (!midiNode)  throw std::invalid_argument("Not a MIDI clip");

    auto result = choc::value::createObject("");
    result.setMember("tickResolution", static_cast<int32_t>(midiNode->tickResolution()));
    result.setMember("bpm", midiNode->clipTempo());

    const auto& words = midiNode->umpEvents();
    const auto& ticks = midiNode->eventTimestampsTicks();
    auto eventsArr = choc::value::createEmptyArray();
    size_t i = 0;
    int32_t evtIdx = 0;
    while (i < words.size()) {
        umppi::Ump u(words[i]);
        int sz = std::max(1, u.getSizeInInts());
        auto evt = choc::value::createObject("");
        evt.setMember("eventIndex", evtIdx);
        evt.setMember("tick", choc::value::createInt64(
            static_cast<int64_t>(i < ticks.size() ? ticks[i] : 0)));
        auto wordsArr = choc::value::createEmptyArray();
        for (int w = 0; w < sz && i + static_cast<size_t>(w) < words.size(); ++w)
            wordsArr.addArrayElement(choc::value::createInt64(
                static_cast<int64_t>(static_cast<uint64_t>(words[i + static_cast<size_t>(w)]))));
        evt.setMember("words", wordsArr);
        eventsArr.addArrayElement(evt);
        i += static_cast<size_t>(sz);
        ++evtIdx;
    }
    result.setMember("events", eventsArr);
    return result;
}

bool uapmd::AppModel::addUmpEventToClip(int32_t trackIndex, int32_t clipId,
                                         uint64_t tick,
                                         std::vector<uint32_t> wordsIn,
                                         std::string& error)
{
    if (wordsIn.empty()) { error = "words must not be empty"; return false; }
    // Validate word count matches UMP message type
    umppi::Ump u(wordsIn[0]);
    int expectedSz = std::max(1, u.getSizeInInts());
    if (static_cast<int>(wordsIn.size()) != expectedSz) {
        error = "words count (" + std::to_string(wordsIn.size()) +
                ") does not match UMP message type (expected " +
                std::to_string(expectedSz) + ")";
        return false;
    }
    return modifyMidiClipUmp(trackIndex, clipId,
        [tick, &wordsIn](std::vector<uapmd_ump_t>& w,
                         std::vector<uint64_t>& t,
                         std::string&) {
            // Find first word index with tick > insertion tick
            size_t insertAt = w.size();
            for (size_t i = 0; i < t.size(); ++i)
                if (t[i] > tick) { insertAt = i; break; }
            w.insert(w.begin() + static_cast<std::ptrdiff_t>(insertAt),
                     wordsIn.begin(), wordsIn.end());
            t.insert(t.begin() + static_cast<std::ptrdiff_t>(insertAt),
                     wordsIn.size(), tick);
            return true;
        }, error);
}

bool uapmd::AppModel::removeUmpEventFromClip(int32_t trackIndex, int32_t clipId,
                                             int32_t eventIndex, std::string& error)
{
    if (eventIndex < 0) { error = "eventIndex must be >= 0"; return false; }
    return modifyMidiClipUmp(trackIndex, clipId,
        [eventIndex](std::vector<uapmd_ump_t>& w,
                     std::vector<uint64_t>& t,
                     std::string& err) {
            // Walk the word stream counting logical events to find the raw word offset
            size_t wordStart = w.size(); // sentinel = not found
            int32_t curEvt = 0;
            size_t i = 0;
            while (i < w.size()) {
                if (curEvt == eventIndex) { wordStart = i; break; }
                umppi::Ump u(w[i]);
                i += static_cast<size_t>(std::max(1, u.getSizeInInts()));
                ++curEvt;
            }
            if (wordStart >= w.size()) {
                err = "eventIndex out of range";
                return false;
            }
            umppi::Ump u(w[wordStart]);
            size_t wordCount = static_cast<size_t>(std::max(1, u.getSizeInInts()));
            w.erase(w.begin() + static_cast<std::ptrdiff_t>(wordStart),
                    w.begin() + static_cast<std::ptrdiff_t>(wordStart + wordCount));
            if (!t.empty()) {
                size_t tickEnd = std::min(wordStart + wordCount, t.size());
                t.erase(t.begin() + static_cast<std::ptrdiff_t>(wordStart),
                        t.begin() + static_cast<std::ptrdiff_t>(tickEnd));
            }
            return true;
        }, error);
}

bool uapmd::AppModel::getClipAudioEvents(int32_t trackIndex, int32_t clipId,
                                         std::vector<uapmd::ClipMarker>& markers,
                                         std::vector<uapmd::AudioWarpPoint>& audioWarps,
                                         std::string& error) const
{
    if (trackIndex == uapmd::kMasterTrackIndex) {
        markers = master_track_markers_;
        audioWarps.clear();
        return true;
    }

    auto tracks = const_cast<AppModel*>(this)->getTimelineTracks();
    if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(tracks.size()) || !tracks[trackIndex]) {
        error = "Track not found";
        return false;
    }

    auto* clip = tracks[trackIndex]->clipManager().getClip(clipId);
    if (!clip) {
        error = "Clip not found";
        return false;
    }

    markers = clip->markers;
    audioWarps = clip->audioWarps;
    return true;
}

bool uapmd::AppModel::setMasterTrackMarkersWithValidation(std::vector<uapmd::ClipMarker> markers, std::string& error)
{
    std::unordered_set<std::string> ids;
    for (size_t i = 0; i < markers.size(); ++i) {
        if (markers[i].markerId.empty())
            markers[i].markerId = std::format("marker_{}", i + 1);
        if (!ids.insert(markers[i].markerId).second) {
            error = std::format("Duplicate marker ID '{}'", markers[i].markerId);
            return false;
        }
    }

    auto clipLookup = buildClipReferenceMap(getTimelineTracks());
    if (!validateMarkerGraphAcyclic(kMasterMarkerReferenceId, markers, clipLookup, markers)) {
        error = "Recursive marker references are not allowed.";
        return false;
    }

    setMasterTrackMarkers(std::move(markers));
    return true;
}

bool uapmd::AppModel::setClipAudioEvents(int32_t trackIndex, int32_t clipId,
                                         std::vector<uapmd::ClipMarker> markers,
                                         std::vector<uapmd::AudioWarpPoint> audioWarps,
                                         std::string& error)
{
    if (trackIndex == uapmd::kMasterTrackIndex)
        return setMasterTrackMarkersWithValidation(std::move(markers), error);

    auto tracks = getTimelineTracks();
    if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(tracks.size()) || !tracks[trackIndex]) {
        error = "Track unavailable.";
        return false;
    }

    auto& track = tracks[trackIndex];
    auto* clip = track->clipManager().getClip(clipId);
    if (!clip) {
        error = "Clip not found.";
        return false;
    }
    if (clip->clipType != uapmd::ClipType::Audio) {
        error = "Selected clip is not an audio clip.";
        return false;
    }
    if (clip->filepath.empty()) {
        error = "Audio clip has no source file path.";
        return false;
    }

    std::unordered_set<std::string> markerIds;
    for (size_t i = 0; i < markers.size(); ++i) {
        if (markers[i].markerId.empty())
            markers[i].markerId = std::format("marker_{}", i + 1);
        if (!markerIds.insert(markers[i].markerId).second) {
            error = std::format("Duplicate marker ID '{}'.", markers[i].markerId);
            return false;
        }
    }

    auto clipLookup = buildClipReferenceMap(tracks);
    auto proposedMasterMarkers = master_track_markers_;
    if (!validateMarkerGraphAcyclic(clip->referenceId, markers, clipLookup, proposedMasterMarkers)) {
        error = "Recursive marker references are not allowed.";
        return false;
    }

    for (const auto& warp : audioWarps) {
        if (!std::isfinite(warp.speedRatio) || warp.speedRatio <= 0.0) {
            error = "Warp speed ratio must be positive and finite.";
            return false;
        }

        const auto reference = warp.timeReference(clip->referenceId, kMasterMarkerReferenceId);
        if (reference.type == uapmd::TimeReferenceType::Point) {
            std::string ownerReferenceId;
            std::string markerId;
            if (!uapmd::TimeReference::parsePointReferenceId(reference.referenceId, ownerReferenceId, markerId)) {
                error = "Warp point reference is invalid.";
                return false;
            }
            if (ownerReferenceId == clip->referenceId && !markerIds.contains(markerId)) {
                error = std::format("Warp references unknown local marker ID '{}'.", markerId);
                return false;
            }
        }
    }

    const int64_t authoredDuration = clip->durationSamples;
    const bool preserveClipDuration = std::ranges::any_of(markers, [&](const auto& marker) {
            return referencesThisClipEnd(marker, clip->referenceId);
        }) || std::ranges::any_of(audioWarps, [&](const auto& warp) {
            return referencesThisClipEnd(warp, clip->referenceId);
        });

    auto reader = uapmd::createAudioFileReaderFromPath(clip->filepath);
    if (!reader) {
        error = "Could not reopen the audio file for warp rebuild.";
        return false;
    }

    auto targetClip = *clip;
    targetClip.markers = markers;
    clipLookup[targetClip.referenceId] = targetClip;
    auto resolvedWarps = resolveAudioWarpPoints(targetClip, audioWarps, clipLookup, master_track_markers_);
    auto newNode = std::make_unique<uapmd::AudioFileSourceNode>(
        clip->sourceNodeInstanceId,
        std::move(reader),
        static_cast<double>(sampleRate()),
        resolvedWarps
    );

    if (!track->clipManager().setClipMarkers(clipId, markers)) {
        error = "Failed to update clip markers.";
        return false;
    }
    if (!track->clipManager().setAudioWarps(clipId, audioWarps)) {
        error = "Failed to update audio warp points.";
        return false;
    }
    if (!track->replaceClipSourceNode(clipId, std::move(newNode))) {
        error = "Failed to rebuild warped audio source.";
        return false;
    }
    if (preserveClipDuration)
        track->clipManager().resizeClip(clipId, authoredDuration);

    resolveAllClipAnchorsInAppModel(*this);
    return true;
}

uapmd::AppModel::ClipAddResult uapmd::AppModel::createEmptyMidiClip(
    int32_t trackIndex, int64_t positionSamples,
    uint32_t tickResolution, double bpm)
{
    uapmd::TimelinePosition pos{};
    pos.samples = positionSamples;
    return addMidiClipToTrack(trackIndex, pos, {}, {}, tickResolution, bpm, {}, {});
}

int32_t uapmd::AppModel::addDeviceInputToTrack(
    int32_t trackIndex,
    const std::vector<uint32_t>& channelIndices
) {
    auto timelineTracks = sequencer_.engine()->timeline().tracks();
    if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(timelineTracks.size()))
        return -1;

    // Engine doesn't expose a next_source_node_id_ for DeviceInput nodes yet;
    // use a local counter for device input nodes in AppModel scope.
    int32_t sourceNodeId = next_source_node_id_++;
    uint32_t channelCount = channelIndices.empty() ? 2 : static_cast<uint32_t>(channelIndices.size());

    auto sourceNode = std::make_unique<uapmd::DeviceInputSourceNode>(
        sourceNodeId,
        channelCount,
        channelIndices
    );

    if (timelineTracks[trackIndex]->addDeviceInputSource(std::move(sourceNode)))
        return sourceNodeId;

    return -1;
}

std::vector<uapmd::TimelineTrack*> uapmd::AppModel::getTimelineTracks() {
    return sequencer_.engine()->timeline().tracks();
}

uapmd::TimelineTrack* uapmd::AppModel::getMasterTimelineTrack() {
    return sequencer_.engine()->timeline().masterTimelineTrack();
}

uapmd::AppModel::MasterTrackSnapshot uapmd::AppModel::buildMasterTrackSnapshot() {
    auto engineSnapshot = sequencer_.engine()->timeline().buildMasterTrackSnapshot();
    MasterTrackSnapshot snapshot;
    snapshot.maxTimeSeconds = engineSnapshot.maxTimeSeconds;
    for (auto& p : engineSnapshot.tempoPoints) {
        MasterTrackSnapshot::TempoPoint point;
        point.timeSeconds = p.timeSeconds;
        point.tickPosition = p.tickPosition;
        point.bpm = p.bpm;
        snapshot.tempoPoints.push_back(point);
    }
    for (auto& p : engineSnapshot.timeSignaturePoints) {
        MasterTrackSnapshot::TimeSignaturePoint point;
        point.timeSeconds = p.timeSeconds;
        point.tickPosition = p.tickPosition;
        point.signature = p.signature;
        snapshot.timeSignaturePoints.push_back(point);
    }
    return snapshot;
}

uapmd::AppModel::TimelineContentBounds uapmd::AppModel::timelineContentBounds() const {
    TimelineContentBounds bounds;
    auto engineBounds = sequencer_.engine()->timeline().calculateContentBounds();
    bounds.hasContent = engineBounds.hasContent;
    bounds.startSeconds = engineBounds.firstSeconds;
    bounds.endSeconds = engineBounds.lastSeconds;
    bounds.durationSeconds = engineBounds.durationSeconds();
    return bounds;
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

    // Clear clips via engine (which owns the timeline tracks)
    auto timelineTracks = sequencer_.engine()->timeline().tracks();
    if (trackIndex >= 0 && trackIndex < static_cast<int32_t>(timelineTracks.size())) {
        timelineTracks[trackIndex]->clipManager().clearAll();
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

void uapmd::AppModel::saveProjectToDocument(DocumentHandle handle,
                                            IDocumentProvider::WriteCallback callback)
{
    auto* provider = documentProvider();
    if (!provider) {
        if (callback)
            callback({false, "Document provider unavailable"});
        return;
    }

    std::string tempDirError;
    auto tempDir = createTempProjectDirectory(tempDirError);
    if (!tempDir) {
        if (callback)
            callback({false, tempDirError});
        return;
    }
    auto stage = std::make_shared<ScopedTempDir>(std::move(*tempDir));

    const auto stagePath = stage->get() / std::filesystem::path("project.uapmd");

    saveProject(stagePath,
                [provider, handle = std::move(handle), callback = std::move(callback), stage](ProjectResult saveResult) mutable {
                    if (!saveResult.success) {
                        if (callback)
                            callback({false, saveResult.error});
                        return;
                    }

                    std::vector<uint8_t> archive;
                    std::string archiveError;
                    if (!ProjectArchive::createArchive(stage->get(), archive, archiveError)) {
                        if (callback)
                            callback({false, archiveError});
                        return;
                    }

                    provider->writeDocument(std::move(handle), std::move(archive),
                                            [callback = std::move(callback), stage](DocumentIOResult ioResult) mutable {
                                                if (callback)
                                                    callback(ioResult);
                                            });
                });
}

uapmd::AppModel::ProjectResult uapmd::AppModel::loadProjectFromResolvedPath(
    const std::filesystem::path& projectFile)
{
    ProjectResult failureResult;
    // Keep the previously loaded archive data alive unless we successfully load a new project.
    std::error_code existsEc;
    if (projectFile.empty() || !std::filesystem::exists(projectFile, existsEc)) {
        failureResult.error = existsEc ? existsEc.message() : "Project file is unavailable.";
        return failureResult;
    }

    if (!ProjectArchive::isArchive(projectFile)) {
        auto result = loadProject(projectFile);
        if (result.success) {
            activeProjectTempDir_.reset();
        }
        return result;
    }

    std::string tempDirError;
    auto tempDir = createTempProjectDirectory(tempDirError);
    if (!tempDir) {
        failureResult.error = tempDirError;
        return failureResult;
    }
    auto stage = std::make_unique<ScopedTempDir>(std::move(*tempDir));

    auto extract = ProjectArchive::extractArchive(projectFile, stage->get());
    if (!extract.success) {
        failureResult.error = extract.error;
        return failureResult;
    }
    if (extract.projectFile.empty()) {
        failureResult.error = "Project archive missing .uapmd file.";
        return failureResult;
    }

    auto result = loadProject(extract.projectFile);
    if (result.success) {
        markLoadedArchiveClipsNeedsFileSave(*this);
        activeProjectTempDir_ = std::move(stage);
    }
    return result;
}

uapmd::AppModel::ProjectResult uapmd::AppModel::saveProjectSync(const std::filesystem::path& projectFile) {
    auto promise = std::make_shared<std::promise<ProjectResult>>();
    auto future = promise->get_future();
    saveProject(projectFile,
                [promise](ProjectResult result) {
                    promise->set_value(std::move(result));
                });
    return future.get();
}

void uapmd::AppModel::saveProject(const std::filesystem::path& projectFile, ProjectSaveCallback callback) {
    auto operation = std::make_shared<PendingProjectSaveContext>();
    operation->project_file = projectFile;
    operation->callback = std::move(callback);

    auto complete = [operation](ProjectResult result) mutable {
        if (!operation->callback)
            return;
        auto callback = std::move(operation->callback);
        callback(std::move(result));
    };

    if (projectFile.empty()) {
        complete(ProjectResult{false, "Project path is empty"});
        return;
    }

    try {
        operation->project_dir = projectFile.parent_path();
        if (!operation->project_dir.empty())
            std::filesystem::create_directories(operation->project_dir);
        auto clipDir = operation->project_dir / "clips";
        operation->plugin_state_dir = operation->project_dir / "plugin_states";

        operation->project = uapmd::UapmdProjectData::create();
        auto* sequencerEngine = sequencer_.engine();
        if (sequencerEngine) {
            operation->project->outputAlignmentMonitoringPolicy(
                outputAlignmentMonitoringPolicyToProjectString(
                    sequencerEngine->outputAlignmentMonitoringPolicy()));
        }
        auto sequencerTracks = sequencerEngine ? sequencerEngine->tracks() : std::vector<uapmd::SequencerTrack*>{};
        auto timelineTracks = getTimelineTracks();
        struct SerializedTrackClips {
            int32_t trackIndex{0};
            std::vector<uapmd::ClipData> clips;
        };
        std::unordered_map<std::string, uapmd::UapmdProjectClipData*> serializedClipLookup;
        std::vector<SerializedTrackClips> serializedTracks;

        size_t midiExportCounter = 0;

        for (size_t trackIndex = 0; trackIndex < timelineTracks.size(); ++trackIndex) {
            if (hidden_tracks_.contains(static_cast<int32_t>(trackIndex)))
                continue;

            auto* timelineTrack = timelineTracks[trackIndex];
            if (!timelineTrack)
                continue;

            auto projectTrack = uapmd::UapmdProjectTrackData::create();
            auto clips = timelineTrack->clipManager().getAllClips();
            std::sort(clips.begin(), clips.end(), [](const uapmd::ClipData& a, const uapmd::ClipData& b) {
                return a.clipId < b.clipId;
            });
            serializedTracks.push_back(SerializedTrackClips{
                static_cast<int32_t>(trackIndex),
                clips});

            for (const auto& clip : clips) {
                auto projectClip = uapmd::UapmdProjectClipData::create();
                projectClip->clipType(clip.clipType == uapmd::ClipType::Midi ? "midi" : "audio");
                projectClip->tickResolution(clip.tickResolution);
                projectClip->markers(clip.markers);
                projectClip->audioWarps(clip.audioWarps);

                std::filesystem::path clipPath = clip.filepath;
                if (clip.clipType == uapmd::ClipType::Midi) {
                    bool needsExport = clip.needsFileSave || clipPath.empty() || !std::filesystem::exists(clipPath);
                    if (needsExport) {
                        std::filesystem::create_directories(clipDir);
                        auto exportName = std::format("track{}_clip_{}_{}.midi2",
                                                      trackIndex,
                                                      clip.clipId,
                                                      midiExportCounter++);
                        auto exportPath = clipDir / exportName;

                        auto sourceNode = timelineTrack->getSourceNode(clip.sourceNodeInstanceId);
                        auto* midiNode = dynamic_cast<uapmd::MidiClipSourceNode*>(sourceNode.get());
                        if (!midiNode) {
                            complete(ProjectResult{false, std::format("Clip {} on track {} is missing MIDI data",
                                                                      clip.clipId, trackIndex)});
                            return;
                        }

                        std::string writeError;
                        auto clipUmps = buildSmf2ClipFromMidiNode(*midiNode, false);
                        if (!uapmd::Smf2ClipReaderWriter::write(exportPath, clipUmps, &writeError)) {
                            complete(ProjectResult{false, std::move(writeError)});
                            return;
                        }
                        clipPath = exportPath;
                    } else {
                        clipPath = std::filesystem::absolute(clipPath);
                    }
                } else {
                    if (clip.needsFileSave) {
                        if (clipPath.empty()) {
                            complete(ProjectResult{false, std::format("Clip {} on track {} has no source audio to save",
                                                                      clip.clipId, trackIndex)});
                            return;
                        }

                        auto sourcePath = std::filesystem::absolute(clipPath);
                        if (!std::filesystem::exists(sourcePath)) {
                            complete(ProjectResult{false, std::format("Clip {} on track {} is missing its audio file",
                                                                      clip.clipId, trackIndex)});
                            return;
                        }

                        std::error_code dirEc;
                        std::filesystem::create_directories(clipDir, dirEc);
                        if (dirEc) {
                            complete(ProjectResult{false, std::format("Failed to create clip directory: {}", dirEc.message())});
                            return;
                        }

                        auto originalName = sourcePath.filename().string();
                        if (originalName.empty())
                            originalName = std::format("clip{}_audio.wav", clip.clipId);
                        auto destPath = clipDir / std::format("track{}_{}", trackIndex, originalName);

                        std::error_code copyEc;
                        std::filesystem::copy_file(sourcePath, destPath, std::filesystem::copy_options::overwrite_existing, copyEc);
                        if (copyEc) {
                            complete(ProjectResult{false, std::format("Failed to store audio clip {}: {}",
                                                                      clip.clipId, copyEc.message())});
                            return;
                        }

                        clipPath = destPath;
                    } else if (!clipPath.empty()) {
                        clipPath = std::filesystem::absolute(clipPath);
                    }
                }

                if (!clipPath.empty() && !operation->project_dir.empty())
                    clipPath = makeRelativePath(operation->project_dir, clipPath);

                projectClip->file(clipPath);

                serializedClipLookup[clip.referenceId] = projectClip.get();
                projectTrack->clips().push_back(std::move(projectClip));
            }

            uapmd::SequencerTrack* sequencerTrack = (sequencerEngine && trackIndex < sequencerTracks.size())
                ? sequencerTracks[trackIndex]
                : nullptr;
            if (auto graphData = createSerializedPluginGraph(
                    sequencerTrack,
                    sequencerEngine,
                    &operation->pending_states,
                    std::format("track{}", trackIndex))) {
                projectTrack->graph(std::move(graphData));
            }

            bool hasClips = !projectTrack->clips().empty();
            bool hasPlugins = projectTrack->graph() && !projectTrack->graph()->plugins().empty();
            if (!hasClips && !hasPlugins)
                continue;

            operation->project->addTrack(std::move(projectTrack));
        }

        if (auto* masterTrack = operation->project->masterTrack()) {
            masterTrack->clips().clear();
            masterTrack->markers(master_track_markers_);
            if (auto* masterTimelineTrack = getMasterTimelineTrack()) {
                auto clips = masterTimelineTrack->clipManager().getAllClips();
                std::sort(clips.begin(), clips.end(), [](const uapmd::ClipData& a, const uapmd::ClipData& b) {
                    return a.clipId < b.clipId;
                });
                serializedTracks.push_back(SerializedTrackClips{
                    uapmd::kMasterTrackIndex,
                    clips});

                for (const auto& clip : clips) {
                    if (clip.clipType != uapmd::ClipType::Midi)
                        continue;

                    auto projectClip = uapmd::UapmdProjectClipData::create();
                    projectClip->clipType("midi");
                    projectClip->tickResolution(clip.tickResolution);
                    projectClip->markers(clip.markers);
                    projectClip->audioWarps(clip.audioWarps);

                    std::filesystem::path clipPath = clip.filepath;
                    bool needsExport = clip.needsFileSave || clipPath.empty() || !std::filesystem::exists(clipPath);
                    if (needsExport) {
                        std::filesystem::create_directories(clipDir);
                        auto exportName = std::format("master_clip_{}_{}.midi2",
                                                      clip.clipId,
                                                      midiExportCounter++);
                        auto exportPath = clipDir / exportName;

                        auto sourceNode = masterTimelineTrack->getSourceNode(clip.sourceNodeInstanceId);
                        auto* midiNode = dynamic_cast<uapmd::MidiClipSourceNode*>(sourceNode.get());
                        if (!midiNode) {
                            complete(ProjectResult{false, std::format("Master clip {} is missing MIDI data", clip.clipId)});
                            return;
                        }

                        std::string writeError;
                        auto clipUmps = buildSmf2ClipFromMidiNode(*midiNode, true);
                        if (!uapmd::Smf2ClipReaderWriter::write(exportPath, clipUmps, &writeError)) {
                            complete(ProjectResult{false, std::move(writeError)});
                            return;
                        }
                        clipPath = exportPath;
                    } else {
                        clipPath = std::filesystem::absolute(clipPath);
                    }

                    if (!clipPath.empty() && !operation->project_dir.empty())
                        clipPath = makeRelativePath(operation->project_dir, clipPath);
                    projectClip->file(clipPath);

                    serializedClipLookup[clip.referenceId] = projectClip.get();
                    masterTrack->clips().push_back(std::move(projectClip));
                }
            }

            if (auto graphData = createSerializedPluginGraph(
                    sequencerEngine ? sequencerEngine->masterTrack() : nullptr,
                    sequencerEngine,
                    &operation->pending_states,
                    "master")) {
                masterTrack->graph(std::move(graphData));
            }
        }

        for (const auto& serializedTrack : serializedTracks) {
            for (const auto& clip : serializedTrack.clips) {
                auto clipIt = serializedClipLookup.find(clip.referenceId);
                if (clipIt == serializedClipLookup.end())
                    continue;

                const auto timeReference = clip.timeReference(sampleRate());
                uapmd::UapmdTimelinePosition pos{};
                if (!timeReference.referenceId.empty()) {
                    auto anchorIt = serializedClipLookup.find(timeReference.referenceId);
                    if (anchorIt != serializedClipLookup.end())
                        pos.anchor = anchorIt->second;
                }
                pos.origin = (timeReference.type == uapmd::TimeReferenceType::ContainerEnd)
                    ? uapmd::UapmdAnchorOrigin::End
                    : uapmd::UapmdAnchorOrigin::Start;
                pos.samples = static_cast<uint64_t>(std::max<int64_t>(0,
                    uapmd::TimelinePosition::fromSeconds(timeReference.offset, sampleRate()).samples));
                clipIt->second->position(pos);
            }
        }
    } catch (const std::exception& e) {
        complete(ProjectResult{false, e.what()});
        return;
    }

    auto runNext = std::make_shared<std::function<void()>>();
    *runNext = [operation, complete, runNext]() mutable {
        if (operation->next_pending_state >= operation->pending_states.size()) {
            if (!uapmd::UapmdProjectDataWriter::write(operation->project.get(), operation->project_file)) {
                complete(ProjectResult{false, "Failed to write project file"});
                return;
            }
            complete(ProjectResult{true, {}});
            return;
        }

        auto pending = operation->pending_states[operation->next_pending_state];
        if (!pending.instance) {
            ++operation->next_pending_state;
            (*runNext)();
            return;
        }

        pending.instance->requestState(uapmd::StateContextType::Project, false, nullptr,
                                       [operation, complete, runNext, pending](std::vector<uint8_t> state, std::string error, void* callbackContext) mutable {
                                           if (!error.empty()) {
                                               complete(ProjectResult{false, std::format("Failed to retrieve plugin state for instance {}: {}",
                                                                                          pending.instance_id, error)});
                                               return;
                                           }

                                           std::string writeError;
                                           auto relativePath = writePluginStateBlob(operation->project_dir,
                                                                                    operation->plugin_state_dir,
                                                                                    pending.scope_label,
                                                                                    pending.plugin_order,
                                                                                    pending.instance_id,
                                                                                    state,
                                                                                    writeError);
                                           if (!writeError.empty()) {
                                               complete(ProjectResult{false, std::move(writeError)});
                                               return;
                                           }

                                           if (pending.graph && pending.node_index < pending.graph->nodes.size())
                                               pending.graph->nodes[pending.node_index].state_file = std::move(relativePath);

                                           ++operation->next_pending_state;
                                           (*runNext)();
                                       });
    };
    (*runNext)();
}

uapmd::AppModel::ProjectResult uapmd::AppModel::loadProject(const std::filesystem::path& projectFile) {
    ProjectResult result;

    // Delegate project loading to SequencerEngine (which owns timeline tracks and plugins)
    auto engineResult = sequencer_.engine()->timeline().loadProject(projectFile);
    if (!engineResult.success) {
        result.error = engineResult.error;
        return result;
    }

    master_track_markers_.clear();
    if (auto project = uapmd::UapmdProjectDataReader::read(projectFile)) {
        if (auto* masterTrack = project->masterTrack())
            master_track_markers_ = masterTrack->markers();
        sequencer_.engine()->outputAlignmentMonitoringPolicy(
            outputAlignmentMonitoringPolicyFromProjectString(
                project->outputAlignmentMonitoringPolicy()));
    } else {
        sequencer_.engine()->outputAlignmentMonitoringPolicy(
            OutputAlignmentMonitoringPolicy::LOW_LATENCY_LIVE_INPUT);
    }

    // Notify UI about all tracks that were created
    hidden_tracks_.clear();
    notifyTrackLayoutChanged(TrackLayoutChange{TrackLayoutChange::Type::Cleared, -1});
    auto numTracks = static_cast<int32_t>(sequencer_.engine()->tracks().size());
    for (int32_t i = 0; i < numTracks; ++i)
        notifyTrackLayoutChanged(TrackLayoutChange{TrackLayoutChange::Type::Added, i});

    // Rebuild device entries and notify listeners for each plugin instance
    clearDeviceEntries();
    sequencer().engine()->functionBlockManager()->deleteEmptyDevices();

    auto instanceIds = sequencer_.engine()->pluginHost()->instanceIds();
    for (int32_t instanceId : instanceIds) {
        auto result = registerPluginInstanceInternal(instanceId, std::nullopt);
        if (!result.error.empty()) {
            std::cerr << "Failed to register plugin instance " << instanceId
                      << ": " << result.error << std::endl;
        }
        for (auto& cb : instanceCreated) {
            cb(result);
        }
    }

    result.success = true;
    return result;
}

bool uapmd::AppModel::startRenderToFile(const RenderToFileSettings& settings) {
    if (settings.outputPath.empty() && !settings.outputHandle)
        return false;

    auto job = std::make_shared<RenderJobState>();
    {
        std::scoped_lock jobLock(renderJobMutex_);
        if (activeRenderJob_)
            return false;
        activeRenderJob_ = job;
    }

    {
        std::scoped_lock statusLock(renderStatusMutex_);
        renderStatus_ = {};
        renderStatus_.running = true;
        renderStatus_.outputPath = settings.outputPath;
        renderStatus_.message = "Preparing render...";
    }

    std::thread([this, job, settings]() {
        runRenderToFile(settings, job);
    }).detach();

    return true;
}

void uapmd::AppModel::cancelRenderToFile() {
    std::shared_ptr<RenderJobState> job;
    {
        std::scoped_lock jobLock(renderJobMutex_);
        job = activeRenderJob_;
    }
    if (job)
        job->cancel.store(true, std::memory_order_release);
}

uapmd::AppModel::RenderToFileStatus uapmd::AppModel::getRenderToFileStatus() const {
    std::scoped_lock statusLock(renderStatusMutex_);
    return renderStatus_;
}

void uapmd::AppModel::clearCompletedRenderStatus() {
    std::scoped_lock statusLock(renderStatusMutex_);
    if (renderStatus_.completed && !renderStatus_.running) {
        renderStatus_.completed = false;
        renderStatus_.success = false;
        renderStatus_.progress = 0.0;
        renderStatus_.renderedSeconds = 0.0;
        renderStatus_.message.clear();
        renderStatus_.outputPath.clear();
    }
}

void uapmd::AppModel::runRenderToFile(RenderToFileSettings settings, std::shared_ptr<RenderJobState> job) {
    auto releaseJob = [this, &job]() {
        std::scoped_lock jobLock(renderJobMutex_);
        if (activeRenderJob_ == job)
            activeRenderJob_.reset();
    };

    auto updateStatus = [this](auto updater) {
        std::scoped_lock statusLock(renderStatusMutex_);
        updater(renderStatus_);
    };

    auto fail = [&](const std::string& message) {
        updateStatus([&](auto& status) {
            status.running = false;
            status.completed = true;
            status.success = false;
            status.message = message;
        });
        releaseJob();
    };

    std::unique_ptr<ScopedTempDir> renderTempDir;
    std::filesystem::path rendererOutputPath = settings.outputPath;

    if (settings.outputHandle) {
        std::string tempDirError;
        auto tempDir = createTempProjectDirectory(tempDirError);
        if (!tempDir) {
            std::string err = tempDirError.empty()
                ? "Unable to allocate temporary render directory."
                : std::format("Unable to allocate temporary render directory: {}", tempDirError);
            fail(err);
            return;
        }
        renderTempDir = std::make_unique<ScopedTempDir>(std::move(*tempDir));
        std::filesystem::path fileName = rendererOutputPath.filename();
        if (fileName.empty() || fileName.string().empty() || fileName == ".")
            fileName = std::filesystem::path("render.wav");
        rendererOutputPath = renderTempDir->get() / fileName;
    }

    if (job->cancel.load(std::memory_order_acquire)) {
        fail("Render canceled.");
        return;
    }

    const int32_t sampleRate = sample_rate_;
    const uint32_t bufferSize = audio_buffer_size_;
    constexpr uint32_t outputChannels = FIXED_CHANNEL_COUNT;

    bool resumeAudio = false;
    bool resumeTransport = false;

    auto restoreRealtime = [&]() {
        if (resumeAudio) {
            sequencer_.startAudio();
            resumeAudio = false;
        }
        if (resumeTransport) {
            transportController_->resume();
            resumeTransport = false;
        }
    };

    try {
        if (transportController_->isPlaying()) {
            transportController_->pause();
            resumeTransport = true;
        }
        if (sequencer_.isAudioPlaying() != 0) {
            sequencer_.stopAudio();
            resumeAudio = true;
        }

        OfflineRenderSettings renderSettings{};
        renderSettings.outputPath = rendererOutputPath;
        renderSettings.startSeconds = settings.startSeconds;
        renderSettings.endSeconds = settings.endSeconds;
        renderSettings.useContentFallback = settings.useContentFallback;
        renderSettings.contentBoundsValid = settings.contentBoundsValid;
        renderSettings.contentStartSeconds = settings.contentStartSeconds;
        renderSettings.contentEndSeconds = settings.contentEndSeconds;
        renderSettings.tailSeconds = settings.tailSeconds;
        renderSettings.enableSilenceStop = settings.enableSilenceStop;
        renderSettings.silenceDurationSeconds = settings.silenceDurationSeconds;
        renderSettings.silenceThresholdDb = settings.silenceThresholdDb;
        renderSettings.sampleRate = sampleRate;
        renderSettings.bufferSize = bufferSize;
        renderSettings.outputChannels = outputChannels;
        renderSettings.umpBufferSize = DEFAULT_UMP_BUFFER_SIZE;

        OfflineRenderCallbacks callbacks{};
        callbacks.onProgress = [&](const OfflineRenderProgress& progress) {
            updateStatus([&](auto& status) {
                status.progress = progress.progress;
                status.renderedSeconds = progress.renderedSeconds;
                status.message = std::format("{:.2f}s / {:.2f}s", progress.renderedSeconds, progress.totalSeconds);
            });
        };
        callbacks.shouldCancel = [&]() {
            return job->cancel.load(std::memory_order_acquire);
        };

        auto result = renderOfflineProject(*sequencer_.engine(), renderSettings, callbacks);
        restoreRealtime();

        if (result.canceled) {
            fail("Render canceled.");
            return;
        }

        if (!result.success) {
            std::string message = result.errorMessage.empty() ? "Render failed." : result.errorMessage;
            fail(message);
            return;
        }

        if (settings.outputHandle) {
            auto* provider = documentProvider();
            if (!provider) {
                fail("Document provider unavailable.");
                return;
            }

            std::error_code sizeEc;
            auto fileSize = std::filesystem::file_size(rendererOutputPath, sizeEc);
            if (sizeEc) {
                fail(std::format("Unable to read rendered file: {}", sizeEc.message()));
                return;
            }
            if (fileSize > static_cast<int64_t>(std::numeric_limits<size_t>::max())) {
                fail("Rendered file is too large to save.");
                return;
            }

            std::vector<uint8_t> renderedData(static_cast<size_t>(fileSize));
            {
                std::ifstream in(rendererOutputPath, std::ios::binary);
                if (!in) {
                    fail("Unable to open rendered file for transfer.");
                    return;
                }
                if (!in.read(reinterpret_cast<char*>(renderedData.data()),
                             static_cast<std::streamsize>(renderedData.size()))) {
                    fail("Unable to read rendered file for transfer.");
                    return;
                }
            }

            auto writePromise = std::make_shared<std::promise<DocumentIOResult>>();
            auto writeFuture = writePromise->get_future();
            provider->writeDocument(
                *settings.outputHandle,
                std::move(renderedData),
                [p = std::move(writePromise)](DocumentIOResult ioResult) mutable {
                    p->set_value(ioResult);
                });
            DocumentIOResult ioResult = writeFuture.get();
            if (!ioResult.success) {
                std::string message = ioResult.error.empty()
                    ? "Failed to save rendered audio."
                    : ioResult.error;
                fail(message);
                return;
            }
        }

        updateStatus([&](auto& status) {
            status.progress = 1.0;
            status.renderedSeconds = result.renderedSeconds;
        });

        updateStatus([&](auto& status) {
            status.running = false;
            status.completed = true;
            status.success = true;
            status.message = std::format("Render complete ({:.2f} s)", status.renderedSeconds);
        });

        releaseJob();
    } catch (const std::exception& e) {
        restoreRealtime();
        fail(e.what());
    }
}
std::string uapmd::AppModel::generateScanReport() {
    auto& scanner = *pluginScanTool_;
    std::unordered_map<std::string, double> bundleDurations;
    {
        std::lock_guard<std::mutex> lock(scanMetricsMutex_);
        bundleDurations = lastScanBundleDurations_;
    }

    struct BundleRow {
        std::string path;
        std::vector<std::string> pluginDescriptions;
    };
    std::map<std::string, BundleRow> bundleRows;

    auto escapeCell = [](const std::string& text) {
        std::string escaped;
        escaped.reserve(text.size());
        for (char c : text) {
            switch (c) {
                case '|': escaped += "\\|"; break;
                case '\n': escaped += "<br>"; break;
                default: escaped.push_back(c); break;
            }
        }
        return escaped;
    };

    std::ostringstream report;
    report << "# Plugin Scan Report\n\n";
    auto formats = scanner.formats();
    for (auto* format : formats) {
        auto entries = scanner.filterByFormat(scanner.catalog().getPlugins(), format->name());
        if (entries.empty())
            continue;
        for (auto* entry : entries) {
            auto bundle = entry->bundlePath().string();
            auto& row = bundleRows[bundle];
            row.path = bundle;
            row.pluginDescriptions.push_back(std::format("{} ({} - {})",
                                                         entry->displayName(),
                                                         format->name(),
                                                         entry->pluginId()));
        }
    }

    std::vector<std::string> timedBundles;
    timedBundles.reserve(bundleDurations.size());
    for (const auto& [bundlePath, duration] : bundleDurations) {
        (void) duration;
        timedBundles.push_back(bundlePath);
    }
    std::sort(timedBundles.begin(), timedBundles.end());
    timedBundles.erase(std::unique(timedBundles.begin(), timedBundles.end()), timedBundles.end());

    if (timedBundles.empty()) {
        report << "No slow-scan bundle timings were recorded.\n";
        return report.str();
    }

    report << "| Bundle | Scan Time (s) | Plugins |\n";
    report << "| --- | --- | --- |\n";
    for (const auto& bundlePath : timedBundles) {
        auto rowIt = bundleRows.find(bundlePath);
        if (rowIt == bundleRows.end())
            continue;
        const auto& row = rowIt->second;
        std::string pluginCell;
        for (size_t i = 0; i < row.pluginDescriptions.size(); ++i) {
            if (i > 0)
                pluginCell += "<br>";
            pluginCell += row.pluginDescriptions[i];
        }
        std::string timeCell = "N/A";
        if (auto it = bundleDurations.find(bundlePath); it != bundleDurations.end() && it->second > 0.0)
            timeCell = std::format("{:.2f}", it->second);
        report << "| " << escapeCell(bundlePath)
               << " | " << escapeCell(timeCell)
               << " | " << escapeCell(pluginCell)
               << " |\n";
    }
    report << "\nSlow-scan bundles: " << timedBundles.size() << "\n";
    return report.str();
}
