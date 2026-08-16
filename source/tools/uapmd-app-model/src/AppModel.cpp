
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
#include <remidy/detail/event-loop.hpp>
#include "uapmd-midi-service/uapmd-midi-service.hpp"
#include "uapmd-data/uapmd-data.hpp"
#include <uapmd-app-model/uapmd-app-model.hpp>

#define DEFAULT_AUDIO_BUFFER_SIZE 1024
#define DEFAULT_UMP_BUFFER_SIZE 65536
#define DEFAULT_SAMPLE_RATE 48000
#define FIXED_CHANNEL_COUNT 2

constexpr uint32_t kDefaultDctpq = 480;
constexpr uint8_t kTempoGroup = 0;
constexpr uint8_t kTempoChannel = 0;

using namespace uapmd_plugin_hosting;
using namespace uapmd_graph;

namespace {

std::unique_ptr<uapmd_app::AppModel> appModelInstance;

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

using namespace uapmd;

namespace uapmd_app {

namespace {

int64_t defaultEmptyMidiClipDurationSamples(int32_t sampleRate,
                                            double bpm,
                                            const std::vector<MidiTimeSignatureChange>& timeSignatureChanges) {
    const double safeBpm = bpm > 0.0 ? bpm : 120.0;
    const int32_t safeSampleRate = std::max(1, sampleRate);
    uint8_t numerator = 4;
    uint8_t denominator = 4;
    if (!timeSignatureChanges.empty()) {
        if (timeSignatureChanges.front().numerator > 0)
            numerator = timeSignatureChanges.front().numerator;
        if (timeSignatureChanges.front().denominator > 0)
            denominator = timeSignatureChanges.front().denominator;
    }

    const double quarterNotes = static_cast<double>(numerator) * 4.0 / static_cast<double>(denominator);
    const double seconds = quarterNotes * 60.0 / safeBpm;
    return std::max<int64_t>(1, static_cast<int64_t>(std::llround(seconds * safeSampleRate)));
}

void markTimelineTrackClipsNeedsFileSave(AppModel& appModel, TimelineTrack* track, int32_t trackIndex) {
    if (!track)
        return;

    auto& timelineFacade = appModel.sequencer().engine()->timeline();
    for (const auto& clip : track->clipManager().getAllClips())
        timelineFacade.setClipNeedsFileSave(
            trackIndex, clip.clipId, true, ProjectMutationOrigin::Internal);
}

void markLoadedArchiveClipsNeedsFileSave(AppModel& appModel) {
    auto timelineTracks = appModel.getTimelineTracks();
    for (size_t i = 0; i < timelineTracks.size(); ++i)
        markTimelineTrackClipsNeedsFileSave(appModel, timelineTracks[i], static_cast<int32_t>(i));
    markTimelineTrackClipsNeedsFileSave(appModel, appModel.getMasterTimelineTrack(), kMasterTrackIndex);
}

bool masterMarkersEqual(
    const std::vector<ClipMarker>& lhs,
    const std::vector<ClipMarker>& rhs) {
    if (lhs.size() != rhs.size())
        return false;
    for (size_t i = 0; i < lhs.size(); ++i) {
        if (lhs[i].markerId != rhs[i].markerId
            || lhs[i].clipPositionOffset != rhs[i].clipPositionOffset
            || lhs[i].referenceType != rhs[i].referenceType
            || lhs[i].referenceClipId != rhs[i].referenceClipId
            || lhs[i].referenceMarkerId != rhs[i].referenceMarkerId
            || lhs[i].name != rhs[i].name)
            return false;
    }
    return true;
}

class MasterMarkersUndoOperation final : public ProjectUndoableOperation {
public:
    using Apply = std::function<bool(const std::vector<ClipMarker>&)>;

    MasterMarkersUndoOperation(
        std::vector<ClipMarker> before,
        std::vector<ClipMarker> after,
        Apply apply)
        : before_(std::move(before))
        , after_(std::move(after))
        , apply_(std::move(apply)) {
    }

    std::string description() const override {
        return "Edit master markers";
    }

    size_t historySizeInBytes() const override {
        auto size = sizeof(*this);
        for (const auto& marker : before_)
            size += sizeof(marker) + marker.markerId.capacity()
                + marker.referenceClipId.capacity()
                + marker.referenceMarkerId.capacity() + marker.name.capacity();
        for (const auto& marker : after_)
            size += sizeof(marker) + marker.markerId.capacity()
                + marker.referenceClipId.capacity()
                + marker.referenceMarkerId.capacity() + marker.name.capacity();
        return size;
    }

    bool hasEffect() const override {
        return !masterMarkersEqual(before_, after_);
    }

    void perform(
        const ProjectUndoExecutionContext&,
        ProjectUndoCompletion completion) override {
        apply(after_, std::move(completion));
    }

    void undo(
        const ProjectUndoExecutionContext&,
        ProjectUndoCompletion completion) override {
        apply(before_, std::move(completion));
    }

    void redo(
        const ProjectUndoExecutionContext&,
        ProjectUndoCompletion completion) override {
        apply(after_, std::move(completion));
    }

private:
    void apply(
        const std::vector<ClipMarker>& markers,
        ProjectUndoCompletion completion) {
        if (apply_ && apply_(markers)) {
            if (completion)
                completion(ProjectUndoResult::success());
            return;
        }
        if (completion)
            completion(ProjectUndoResult::failure("Could not restore master markers"));
    }

    std::vector<ClipMarker> before_{};
    std::vector<ClipMarker> after_{};
    Apply apply_{};
};

} // namespace

class ClipEnablementSerializationExtension final : public ProjectSerializationExtension {
public:
    explicit ClipEnablementSerializationExtension(AppModel& appModel) : app_model_(appModel) {}

    std::string_view extensionId() const override { return "clip-enablement"; }

    bool saveProjectExtensionData(ProjectSerializationWriteContext& context, std::string& error) override {
        std::string manifest{"uapmd-clip-enablement-v1\n"};
        const auto appendTrack = [&manifest](const TimelineTrack* track) {
            if (!track)
                return;
            const auto clips = track->clipManager().getAllClips();
            bool wroteTrack = false;
            for (const auto& clip : clips) {
                if (clip.enabled)
                    continue;
                if (!wroteTrack) {
                    manifest += "track " + track->referenceId() + "\n";
                    wroteTrack = true;
                }
                manifest += "clip " + clip.referenceId + " disabled\n";
            }
        };
        for (const auto* track : app_model_.getTimelineTracks())
            appendTrack(track);
        appendTrack(app_model_.getMasterTimelineTrack());
        return context.writeExtensionFile(extensionId(), "clip-enablement.txt",
            std::vector<uint8_t>(manifest.begin(), manifest.end()), error);
    }

    bool loadProjectExtensionData(ProjectSerializationReadContext& context, std::string& error) override {
        std::string readError;
        const auto bytes = context.readExtensionFile(extensionId(), "clip-enablement.txt", readError);
        if (!bytes)
            return true; // Projects created before clip enablement have every clip enabled.
        std::istringstream input(std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
        std::string line;
        if (!std::getline(input, line) || line != "uapmd-clip-enablement-v1") {
            error = "Unsupported clip enablement manifest version.";
            return false;
        }
        TimelineTrack* currentTrack = nullptr;
        // Tracked alongside the track pointer because clip mutation is
        // addressed by track index through the timeline facade.
        int32_t currentTrackIndex = -1;
        while (std::getline(input, line)) {
            if (line.rfind("track ", 0) == 0) {
                const auto id = line.substr(6);
                currentTrack = nullptr;
                currentTrackIndex = -1;
                const auto tracks = app_model_.getTimelineTracks();
                for (size_t i = 0; i < tracks.size(); ++i)
                    if (tracks[i] && tracks[i]->referenceId() == id) {
                        currentTrack = tracks[i];
                        currentTrackIndex = static_cast<int32_t>(i);
                    }
                if (auto* master = app_model_.getMasterTimelineTrack(); master && master->referenceId() == id) {
                    currentTrack = master;
                    currentTrackIndex = kMasterTrackIndex;
                }
            } else if (currentTrack && line.rfind("clip ", 0) == 0) {
                const auto separator = line.rfind(" disabled");
                if (separator == std::string::npos)
                    continue;
                const auto clipReference = line.substr(5, separator - 5);
                for (const auto& clip : currentTrack->clipManager().getAllClips())
                    if (clip.referenceId == clipReference)
                        app_model_.sequencer().engine()->timeline()
                            .setClipEnabled(
                                currentTrackIndex, clip.clipId, false,
                                ProjectMutationOrigin::Load);
            }
        }
        return true;
    }

private:
    AppModel& app_model_;
};

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

} // namespace uapmd_app

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

std::vector<umppi::Ump> buildMasterTrackSmf2Clip(const uapmd_app::AppModel::MasterTrackSnapshot& snapshot) {
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

uapmd_app::TransportController::TransportController(AppModel* appModel, RealtimeSequencer* sequencer)
    : appModel_(appModel), sequencer_(sequencer) {
}

std::shared_ptr<uapmd_midi_service::MidiIOFeature> uapmd_app::AppModel::createMidiIOFeature(
    std::string apiName, std::string deviceName, std::string manufacturer, std::string version) {
    return createLibreMidiIODevice(apiName, deviceName, manufacturer, version);
}

std::vector<uapmd::MidiPortInfo> uapmd_app::AppModel::getMidiInputPorts() const {
    auto ports = uapmd::getMidiInputPorts();
    const auto devices = getDevices();
    std::erase_if(ports, [&devices](const MidiPortInfo& port) {
        return std::any_of(devices.begin(), devices.end(), [&port](const DeviceEntry& device) {
            return device.state && port.displayName == device.state->label + " In";
        });
    });
    return ports;
}

std::vector<uapmd::MidiPortInfo> uapmd_app::AppModel::getMidiOutputPorts() const {
    auto ports = uapmd::getMidiOutputPorts();
    const auto devices = getDevices();
    std::erase_if(ports, [&devices](const MidiPortInfo& port) {
        return std::any_of(devices.begin(), devices.end(), [&port](const DeviceEntry& device) {
            return device.state && port.displayName == device.state->label + " Out";
        });
    });
    return ports;
}


void uapmd_app::AppModel::instantiate() {
    appModelInstance = std::make_unique<uapmd_app::AppModel>(DEFAULT_AUDIO_BUFFER_SIZE, DEFAULT_UMP_BUFFER_SIZE, DEFAULT_SAMPLE_RATE, defaultDeviceIODispatcher());
}

uapmd_app::AppModel& uapmd_app::AppModel::instance() {
    return *appModelInstance;
}

void uapmd_app::AppModel::cleanupInstance() {
    appModelInstance.reset();
}

uapmd_app::AppModel::AppModel(size_t audioBufferSizeInFrames, size_t umpBufferSizeInBytes, int32_t sampleRate, DeviceIODispatcher* dispatcher) :
        ump_buffer_size_in_bytes_(umpBufferSizeInBytes),
        sequencer_(audioBufferSizeInFrames, umpBufferSizeInBytes, sampleRate, dispatcher),
        pluginScanTool_(uapmd_plugin_hosting::PluginScanTool::create()),
        transportController_(std::make_unique<TransportController>(this, &sequencer_)),
        sample_rate_(sampleRate),
        audio_buffer_size_(static_cast<uint32_t>(audioBufferSizeInFrames)),
        auto_buffer_size_enabled_(sequencer_.useAutoBufferSize()) {
    clip_enablement_extension_ = std::make_unique<ClipEnablementSerializationExtension>(*this);
    sequencer_.engine()->timeline().addProjectSerializationExtension(*clip_enablement_extension_);
    sequencer_.engine()->functionBlockManager()->setMidiIOManager(this);
    plugin_state_change_dispatch_->app_model = this;
    auto dispatch = plugin_state_change_dispatch_;
    plugin_state_change_listener_id_ = sequencer_.engine()->pluginHost()->addPluginStateChangeListener([dispatch](int32_t instanceId) {
        remidy::EventLoop::enqueueTaskOnMainThread([dispatch, instanceId] {
            std::lock_guard lock(dispatch->mutex);
            auto* appModel = dispatch->app_model;
            if (appModel)
                appModel->handlePluginStateChange(instanceId);
        });
    });

    // Start with a few empty tracks for the DAW layout
    // (Timeline state and preprocess callback are now managed by SequencerEngine)
    constexpr int kInitialTrackCount = 3;
    for (int i = 0; i < kInitialTrackCount; ++i)
        sequencer_.engine()->addEmptyTrack();
    clearProjectDirtyState();
}

void uapmd_app::AppModel::notifyUiReady() {
    {
        std::lock_guard<std::mutex> lock(startupScanMutex_);
        uiReady_ = true;
    }
    maybeStartInitialPluginScan();
}

void uapmd_app::AppModel::notifyPersistentStorageReady() {
    {
        std::lock_guard<std::mutex> lock(startupScanMutex_);
        persistentStorageReady_ = true;
    }
    maybeStartInitialPluginScan();
}

void uapmd_app::AppModel::maybeStartInitialPluginScan() {
    std::lock_guard<std::mutex> lock(startupScanMutex_);
    if (!uiReady_ || !persistentStorageReady_)
        return;
    if (initialPluginScanStarted_.exchange(true, std::memory_order_acq_rel))
        return;
    performPluginScanning(false, PluginScanRequest::InProcess, 0.0, true);
}

uapmd_app::AppModel::~AppModel() {
    if (clip_enablement_extension_)
        sequencer_.engine()->timeline().removeProjectSerializationExtension(*clip_enablement_extension_);
    {
        std::lock_guard lock(plugin_state_change_dispatch_->mutex);
        plugin_state_change_dispatch_->app_model = nullptr;
    }
    if (plugin_state_change_listener_id_ != 0)
        sequencer_.engine()->pluginHost()->removePluginStateChangeListener(plugin_state_change_listener_id_);

    shutting_down_ = true;
    audioEngineEnabled_.store(false, std::memory_order_release);
    completeAudioEngineShutdown();

    std::unordered_set<int32_t> removedInstanceIds;
    if (auto* mt = sequencer_.engine()->masterTrack()) {
        auto ids = mt->orderedInstanceIds();
        for (int32_t instanceId : ids)
            if (removedInstanceIds.insert(instanceId).second)
                removePluginInstance(instanceId);
    }
    if (auto* host = sequencer_.engine()->pluginHost()) {
        auto ids = host->instanceIds();
        for (int32_t instanceId : ids)
            if (removedInstanceIds.insert(instanceId).second)
                removePluginInstance(instanceId);
    }

    joinAudioShutdownWorker();
}

bool uapmd_app::AppModel::ensureTrackUsesEditorGraph(int32_t trackIndex) {
    SequencerTrack* track = trackIndex == kMasterTrackIndex
        ? sequencer_.engine()->masterTrack()
        : (trackIndex >= 0 && trackIndex < static_cast<int32_t>(sequencer_.engine()->tracks().size())
            ? sequencer_.engine()->tracks()[static_cast<size_t>(trackIndex)]
            : nullptr);
    if (!track)
        return false;
    if (dynamic_cast<AudioPluginFullDAGraph*>(&track->graph()))
        return true;
    auto graph = AudioPluginFullDAGraph::create(ump_buffer_size_in_bytes_);
    if (!graph)
        return false;
    const bool changed = sequencer_.engine()->timeline().replaceTrackGraphType(
        trackIndex,
        graph->providerId(),
        ump_buffer_size_in_bytes_);
    if (changed)
        markTrackDirty(trackIndex);
    return changed;
}

bool uapmd_app::AppModel::revertTrackToSimpleGraph(int32_t trackIndex) {
    const bool changed = sequencer_.engine()->timeline().replaceTrackGraphType(
        trackIndex, "", ump_buffer_size_in_bytes_);
    if (changed)
        markTrackDirty(trackIndex);
    return changed;
}

bool uapmd_app::AppModel::getTrackGraphConnections(
    int32_t trackIndex,
    std::vector<AudioPluginGraphConnection>& connections,
    std::string& error) const {
    connections.clear();

    SequencerTrack* track = trackIndex == kMasterTrackIndex
        ? sequencer_.engine()->masterTrack()
        : (trackIndex >= 0 && trackIndex < static_cast<int32_t>(sequencer_.engine()->tracks().size())
            ? sequencer_.engine()->tracks()[static_cast<size_t>(trackIndex)]
            : nullptr);
    if (!track) {
        error = "Track not found";
        return false;
    }

    auto* dag = dynamic_cast<AudioPluginFullDAGraph*>(&track->graph());
    if (!dag) {
        error = "Track graph is not a full DAG graph";
        return false;
    }

    connections = dag->connections();
    return true;
}

bool uapmd_app::AppModel::connectTrackGraph(
    int32_t trackIndex,
    const AudioPluginGraphConnection& connection,
    std::string& error) {
    const bool changed = sequencer_.engine()->timeline().connectTrackGraph(
        trackIndex,
        connection,
        error);
    if (changed)
        markTrackDirty(trackIndex);
    return changed;
}

bool uapmd_app::AppModel::disconnectTrackGraphConnection(
    int32_t trackIndex,
    int64_t connectionId,
    std::string& error) {
    const bool changed = sequencer_.engine()->timeline()
        .disconnectTrackGraphConnection(trackIndex, connectionId, error);
    if (changed)
        markTrackDirty(trackIndex);
    return changed;
}

uapmd::IDocumentProvider* uapmd_app::AppModel::documentProvider() {
    if (!documentProvider_) {
        documentProvider_ = createDocumentProvider();
        if (!documentProvider_) {
            std::cerr << "Document provider unavailable; dialogs disabled." << std::endl;
        }
    }
    return documentProvider_.get();
}

void uapmd_app::AppModel::cancelPluginScanning() {
    if (!isScanning_)
        return;
    scanCancelRequested_.store(true, std::memory_order_release);
}

void uapmd_app::AppModel::performPluginScanning(bool forceRescan,
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

            uapmd_plugin_hosting::PluginScanObserver observer;
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
                        ? uapmd_plugin_hosting::ScanMode::Remote
                        : uapmd_plugin_hosting::ScanMode::InProcess;
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

uapmd_app::AppModel::SlowScanProgressState uapmd_app::AppModel::slowScanProgress() const {
    std::lock_guard<std::mutex> lock(slowScanMutex_);
    return slowScanProgress_;
}

std::vector<uapmd_plugin_hosting::BlocklistEntry> uapmd_app::AppModel::pluginBlocklist() const {
    return pluginScanTool_->blocklistEntries();
}

bool uapmd_app::AppModel::unblockPluginFromBlocklist(const std::string& entryId) {
    return pluginScanTool_->unblockBundle(entryId);
}

void uapmd_app::AppModel::clearPluginBlocklist() {
    pluginScanTool_->clearBlocklist();
}

std::string uapmd_app::AppModel::lastPluginScanError() const {
    return pluginScanTool_->lastScanError();
}

uint8_t uapmd_app::AppModel::getInstanceGroup(int32_t instanceId) const {
    auto* engine = sequencer_.engine();
    if (!engine)
        return 0xFF;
    return engine->getInstanceGroup(instanceId);
}

bool uapmd_app::AppModel::setInstanceGroup(int32_t instanceId, uint8_t group) {
    auto* engine = sequencer_.engine();
    if (!engine)
        return false;
    const bool changed =
        engine->timeline().setPluginGroup(instanceId, group);
    if (changed)
        markPluginInstanceTrackDirty(instanceId);
    return changed;
}

// Cancel and reap any in-flight muted shutdown drain. Safe to call from the main
// thread: the worker never blocks on main-thread tasks (it only enqueues them).
void uapmd_app::AppModel::joinAudioShutdownWorker() {
    if (audioShutdownThread_.joinable()) {
        audioShutdownCancel_.store(true, std::memory_order_release);
        audioShutdownThread_.join();
    }
}

// Final phase of the engine-off sequence. Runs on the main thread, after the muted
// drain has let plugin release/reverb tails render out.
void uapmd_app::AppModel::completeAudioEngineShutdown() {
    // Bail out if the engine was re-enabled while draining, or if an earlier queued
    // completion already performed the shutdown.
    if (isAudioEngineEnabled() || pluginsProcessingStopped_)
        return;

    const auto bufferPeriodMs = sample_rate_ > 0
        ? static_cast<int64_t>(audio_buffer_size_ * 1000.0 / sample_rate_) : 10;
    // Silence the output, then let the audio callback run for a couple more cycles
    // so the hardware ring buffer drains with silence before we stop.
    sequencer_.engine()->setEngineActive(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(std::max<int64_t>(30, bufferPeriodMs * 2)));
    sequencer_.stopAudio();
    // With the callback stopped, the plugin formats can complete their (possibly
    // deferred) stop synchronously, deactivating plugins and resetting their DSP
    // state so a restart does not resume stale voices or effect tails.
    auto* host = sequencer_.engine()->pluginHost();
    for (auto id : host->instanceIds())
        host->getInstance(id)->stopProcessing();
    pluginsProcessingStopped_ = true;
    // Finally clear all host-side intermediate buffers (pump ring slots, alignment
    // delay lines, queued events, ...) so nothing stale is replayed on restart.
    sequencer_.engine()->resetProcessingState();
    sequencer_.engine()->setOutputMuted(false);
}

void uapmd_app::AppModel::setAudioEngineEnabled(bool enabled) {
    joinAudioShutdownWorker();

    if (enabled && isAudioEngineEnabled())
        return;
    audioEngineEnabled_.store(enabled, std::memory_order_release);

    auto* host = sequencer_.engine()->pluginHost();
    if (enabled) {
        // If the shutdown drain was cancelled before it reached plugin deactivation,
        // finish it synchronously now: turning the engine back on must ALWAYS start
        // from deactivated, reset plugins so no previous synthesis can resume.
        if (!pluginsProcessingStopped_) {
            audioEngineEnabled_.store(false, std::memory_order_release);
            completeAudioEngineShutdown();
            audioEngineEnabled_.store(true, std::memory_order_release);
        }
        for (auto id : host->instanceIds())
            host->getInstance(id)->startProcessing();
        pluginsProcessingStopped_ = false;
        sequencer_.engine()->setOutputMuted(false);
        sequencer_.engine()->setEngineActive(true);
        if (sequencer_.isAudioPlaying() == 0) {
            if (sequencer_.startAudio() != 0) {
                std::cerr << "Failed to start audio engine" << std::endl;
                audioEngineEnabled_.store(false, std::memory_order_release);
                sequencer_.engine()->setEngineActive(false);
            }
        }
    } else if (sequencer_.isAudioPlaying() != 0) {
        transportController_->stop();
        // stop() requested a note-off flush on every plugin node; it is delivered by
        // the next audio cycles. Mute the device output immediately, but keep the
        // engine processing so the flush lands and the release/reverb tails render
        // out inaudibly — several formats (notably VST3) preserve their DSP state
        // across deactivation, so any audible tail left here would resume on restart.
        sequencer_.engine()->setOutputMuted(true);
        audioShutdownCancel_.store(false, std::memory_order_release);
        audioShutdownThread_ = std::thread([this] {
            constexpr int kPollIntervalMs = 100;
            constexpr int kMaxDrainMs = 8000;
            constexpr double kSilenceThreshold = 0.005;
            constexpr int kTimeDomainSamples = 256;
            float timeDomain[kTimeDomainSamples];
            for (int waited = 0; waited < kMaxDrainMs; waited += kPollIntervalMs) {
                std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
                if (audioShutdownCancel_.load(std::memory_order_acquire))
                    break;
                auto* analyser = sequencer_.engine()->outputAnalyser();
                if (!analyser)
                    break;
                analyser->getFloatTimeDomainData(timeDomain, kTimeDomainSamples);
                double sum = 0.0;
                for (float v : timeDomain)
                    sum += std::abs(v);
                if (sum < kSilenceThreshold)
                    break;
            }
            // Plugin deactivation must run on the main thread (VST3 setActive() etc.).
            // Enqueue — never block on it — so this worker can always be joined from
            // the main thread without deadlocking.
            remidy::EventLoop::enqueueTaskOnMainThread([this] {
                completeAudioEngineShutdown();
            });
        });
    }
}

void uapmd_app::AppModel::toggleAudioEngine() {
    bool desired = !audioEngineEnabled_.load(std::memory_order_acquire);
    setAudioEngineEnabled(desired);
}

void uapmd_app::AppModel::updateAudioDeviceSettings(int32_t sampleRate, uint32_t bufferSize) {
    // These UI settings are forwarded to RealtimeSequencer -> DeviceIODispatcher,
    // which ultimately sets OboeAudioIODevice::preferred_callback_frames_.  That
    // value is the block size the sequencer renders for every plugin regardless
    // of the device's framesPerBurst, so edits here change the host buffer size.
    if (sampleRate > 0)
        sample_rate_ = sampleRate;
    if (bufferSize > 0)
        audio_buffer_size_ = bufferSize;
}

void uapmd_app::AppModel::setAutoBufferSizeEnabled(bool enabled) {
    sequencer_.setUseAutoBufferSize(enabled);
    auto_buffer_size_enabled_ = sequencer_.useAutoBufferSize();
}

bool uapmd_app::AppModel::pauseTransportForPluginMutation() {
    if (!transportController_ || !transportController_->isPlaying())
        return false;

    transportController_->pause();
    return true;
}

void uapmd_app::AppModel::resumeTransportAfterPluginMutation(bool resumeTransport) {
    if (!resumeTransport || !transportController_ || !transportController_->isPaused())
        return;

    transportController_->resume();
}

bool uapmd_app::AppModel::isProjectDirty() const {
    std::lock_guard lock(dirtyStateMutex_);
    return sequencer_.engine()->timeline().hasPendingPluginMutations()
        || project_structure_dirty_ || !dirty_tracks_.empty()
        || sequencer_.engine()->timeline().undoEngine().state().dirty;
}

bool uapmd_app::AppModel::isTrackDirty(int32_t trackIndex) const {
    std::lock_guard lock(dirtyStateMutex_);
    return dirty_tracks_.contains(trackIndex);
}

void uapmd_app::AppModel::markProjectDirty() {
    std::lock_guard lock(dirtyStateMutex_);
    project_structure_dirty_ = true;
}

void uapmd_app::AppModel::markTrackDirty(int32_t trackIndex, bool dirty) {
    {
        std::lock_guard lock(dirtyStateMutex_);
        if (dirty)
            dirty_tracks_.insert(trackIndex);
        else
            dirty_tracks_.erase(trackIndex);
    }
    // This notification is deliberately emitted for every dirtying mutation,
    // not only the clean-to-dirty transition. A frozen cache represents one
    // exact project state and must be revoked again after every later edit.
    if (dirty)
        sequencer_.engine()->frozenTrackManager()
            .projectTrackBecameDirty(trackIndex);
}

void uapmd_app::AppModel::markPluginInstanceTrackDirty(int32_t instanceId) {
    const auto trackIndex = sequencer_.engine()->findTrackIndexForInstance(instanceId);
    if (trackIndex >= 0 || trackIndex == kMasterTrackIndex)
        markTrackDirty(trackIndex);
}

void uapmd_app::AppModel::handlePluginStateChange(int32_t instanceId) {
    // State restoration while a frozen track is rendering can make the plugin
    // report a change. It is not a document edit and must not cancel or loop a
    // freeze render.
    if (sequencer_.engine()->frozenTrackManager().isInstanceBusy(instanceId))
        return;
    markPluginInstanceTrackDirty(instanceId);
}

void uapmd_app::AppModel::clearProjectDirtyState() {
    {
        std::lock_guard lock(dirtyStateMutex_);
        project_structure_dirty_ = false;
        dirty_tracks_.clear();
    }
}

uapmd::ProjectUndoState uapmd_app::AppModel::historyState() const {
    auto state = sequencer_.engine()->timeline().undoEngine().state();
    // Plug-in creation/removal captures plug-in state asynchronously before its
    // history entry can be committed.  Expose that interval as a busy history
    // state so keyboard shortcuts and the command menu cannot race the capture.
    if (sequencer_.engine()->timeline().hasPendingPluginMutations()) {
        state.busy = true;
        state.canUndo = false;
        state.canRedo = false;
        state.dirty = true;
    }
    return state;
}

std::unordered_set<int32_t> uapmd_app::AppModel::currentPluginInstanceIds() const {
    std::unordered_set<int32_t> ids;
    if (auto* host = sequencer_.engine()->pluginHost()) {
        const auto hostedIds = host->instanceIds();
        ids.insert(hostedIds.begin(), hostedIds.end());
    }
    return ids;
}

void uapmd_app::AppModel::reconcileAfterHistoryMutation(
    const std::unordered_set<int32_t>& previousPluginInstanceIds) {
    const auto currentIds = currentPluginInstanceIds();
    for (const auto instanceId : previousPluginInstanceIds)
        if (!currentIds.contains(instanceId))
            forgetRemovedPluginInstance(instanceId);

    for (const auto instanceId : currentIds) {
        if (previousPluginInstanceIds.contains(instanceId))
            continue;
        auto result = registerPluginInstanceInternal(instanceId, std::nullopt);
        if (!result.error.empty())
            std::cerr << "Failed to register restored plugin instance "
                      << instanceId << ": " << result.error << std::endl;
        for (auto& cb : instanceCreated)
            cb(result);
    }

    notifyTrackLayoutChanged(TrackLayoutChange{TrackLayoutChange::Type::Cleared, -1});
    const auto numTracks = static_cast<int32_t>(sequencer_.engine()->tracks().size());
    for (int32_t i = 0; i < numTracks; ++i) {
        notifyTrackLayoutChanged(TrackLayoutChange{TrackLayoutChange::Type::Added, i});
        sequencer_.engine()->frozenTrackManager().projectTrackBecameDirty(i);
    }
}

void uapmd_app::AppModel::undo(HistoryMutationCallback callback) {
    if (!callback)
        return;
    if (sequencer_.engine()->timeline().hasPendingPluginMutations()) {
        callback("A plug-in mutation is still completing");
        return;
    }
    const auto previousPluginInstanceIds = currentPluginInstanceIds();
    const bool resumeTransportAfterMutation = pauseTransportForPluginMutation();
    sequencer_.engine()->timeline().undoEngine().undo(
        [this, previousPluginInstanceIds, resumeTransportAfterMutation,
         callback = std::move(callback)](uapmd::ProjectUndoResult result) mutable {
            resumeTransportAfterPluginMutation(resumeTransportAfterMutation);
            if (!result.succeeded()) {
                callback(std::move(result.error));
                return;
            }
            reconcileAfterHistoryMutation(previousPluginInstanceIds);
            callback({});
        });
}

void uapmd_app::AppModel::redo(HistoryMutationCallback callback) {
    if (!callback)
        return;
    if (sequencer_.engine()->timeline().hasPendingPluginMutations()) {
        callback("A plug-in mutation is still completing");
        return;
    }
    const auto previousPluginInstanceIds = currentPluginInstanceIds();
    const bool resumeTransportAfterMutation = pauseTransportForPluginMutation();
    sequencer_.engine()->timeline().undoEngine().redo(
        [this, previousPluginInstanceIds, resumeTransportAfterMutation,
         callback = std::move(callback)](uapmd::ProjectUndoResult result) mutable {
            resumeTransportAfterPluginMutation(resumeTransportAfterMutation);
            if (!result.succeeded()) {
                callback(std::move(result.error));
                return;
            }
            reconcileAfterHistoryMutation(previousPluginInstanceIds);
            callback({});
        });
}

void uapmd_app::AppModel::createPluginInstanceAsync(const std::string& format,
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
    const bool resumeTransportAfterMutation = pauseTransportForPluginMutation();

    auto instantiateCallback = [this, config, pluginName, completionCallback, resumeTransportAfterMutation](int32_t instanceId, int32_t trackIndex, std::string error) {
        PluginInstanceResult result;
        result.instanceId = instanceId;
        result.pluginName = pluginName;
        result.error = std::move(error);

        if (!result.error.empty() || instanceId < 0) {
            resumeTransportAfterPluginMutation(resumeTransportAfterMutation);
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
        if (!result.error.empty()) {
            resumeTransportAfterPluginMutation(resumeTransportAfterMutation);
            if (completionCallback)
                completionCallback(result);
            for (auto& cb : instanceCreated)
                cb(result);
            return;
        }

        sequencer_.engine()->timeline().recordPluginInstanceAddition(
            instanceId,
            uapmd::ProjectMutationOrigin::User,
            [this,
             result = std::move(result),
             trackIndex,
             completionCallback,
             resumeTransportAfterMutation](uapmd::ProjectUndoResult historyResult) mutable {
                auto completed = std::move(result);
                if (!historyResult.succeeded()) {
                    completed.error = historyResult.error.empty()
                        ? "Could not record plug-in creation in undo history"
                        : std::move(historyResult.error);
                    sequencer_.engine()->removePluginInstance(completed.instanceId);
                    forgetRemovedPluginInstance(completed.instanceId);
                }
                if (completed.error.empty())
                    markTrackDirty(trackIndex);
                resumeTransportAfterPluginMutation(resumeTransportAfterMutation);
                if (completionCallback)
                    completionCallback(completed);
                for (auto& cb : instanceCreated)
                    cb(completed);
            });
    };

    if (!targetMasterTrack) {
        if (trackIndex < 0) {
            // Kept on the raw path until plugin insertion itself participates
            // in the same compound history step. Recording the empty track now
            // would make redo restore it without the subsequently added plugin.
            trackIndex = addTrackLegacy();
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

    sequencer_.engine()->addPluginToTrack(
        targetMasterTrack ? kMasterTrackIndex : trackIndex,
        formatCopy,
        pluginIdCopy,
        std::move(instantiateCallback));
}

uapmd_app::AppModel::PluginInstanceResult uapmd_app::AppModel::registerPluginInstanceInternal(
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
        auto stateResult = loadPluginStateSync(
            instanceId,
            configOverride->stateFile.string(),
            uapmd::ProjectMutationOrigin::Internal);
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

void uapmd_app::AppModel::clearDeviceEntries() {
    std::lock_guard lock(devicesMutex_);
    devices_.clear();
    nextDeviceId_ = 1;
}

void uapmd_app::AppModel::forgetRemovedPluginInstance(int32_t instanceId) {
    {
        std::lock_guard lock(devicesMutex_);
        for (auto it = devices_.begin(); it != devices_.end(); ++it) {
            auto state = it->state;
            if (!state)
                continue;
            std::lock_guard guard(state->mutex);
            if (!state->pluginInstances.contains(instanceId))
                continue;
            devices_.erase(it);
            break;
        }
    }
    if (!shutting_down_)
        for (auto& cb : instanceRemoved)
            cb(instanceId);
}

void uapmd_app::AppModel::removePluginInstance(int32_t instanceId) {
    if (sequencer_.engine()->frozenTrackManager().isInstanceBusy(instanceId))
        return;
    const auto trackIndex =
        sequencer_.engine()->findTrackIndexForInstance(instanceId);
    const bool resumeTransportAfterMutation = pauseTransportForPluginMutation();

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

    sequencer_.engine()->timeline().removePluginInstance(
        instanceId,
        (shutting_down_ || project_load_in_progress_)
            ? uapmd::ProjectMutationOrigin::Internal
            : uapmd::ProjectMutationOrigin::User,
        [this, instanceId, trackIndex, resumeTransportAfterMutation](uapmd::ProjectUndoResult result) {
            if (result.succeeded()) {
                if (!shutting_down_) {
                    if (trackIndex >= 0 || trackIndex == kMasterTrackIndex)
                        markTrackDirty(trackIndex);
                    else
                        markProjectDirty();
                }
                sequencer().engine()->functionBlockManager()->deleteEmptyDevices();
                if (!shutting_down_) {
                    for (auto& cb : instanceRemoved)
                        cb(instanceId);
                }
            }
            resumeTransportAfterPluginMutation(resumeTransportAfterMutation);
        });
}

void uapmd_app::AppModel::enableUmpDevice(int32_t instanceId, const std::string& deviceName) {
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

void uapmd_app::AppModel::disableUmpDevice(int32_t instanceId) {
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
    if (!shutting_down_) {
        for (auto& cb : disableDeviceCompleted) {
            cb(result);
        }
    }
}

void uapmd_app::AppModel::requestShowPluginUI(int32_t instanceId) {
    if (sequencer_.engine()->frozenTrackManager().isInstanceBusy(instanceId))
        return;
    // Trigger callbacks - MainWindow will handle preparing window and calling showPluginUI()
    for (auto& cb : uiShowRequested) {
        cb(instanceId);
    }
}

void uapmd_app::AppModel::requestShowInstanceDetails(int32_t instanceId) {
    if (sequencer_.engine()->frozenTrackManager().isInstanceBusy(instanceId))
        return;
    for (auto& cb : instanceDetailsShowRequested)
        cb(instanceId);
}

void uapmd_app::AppModel::requestShowTrackGraph(int32_t trackIndex) {
    if (sequencer_.engine()->frozenTrackManager().isTrackBusy(trackIndex))
        return;
    for (auto& cb : trackGraphShowRequested)
        cb(trackIndex);
}

void uapmd_app::AppModel::showPluginUI(int32_t instanceId, bool needsCreate, bool isFloating, void* parentHandle, std::function<bool(uint32_t, uint32_t)> resizeHandler) {
    UIStateResult result;
    result.instanceId = instanceId;

    if (sequencer_.engine()->frozenTrackManager().isInstanceBusy(instanceId)) {
        result.success = false;
        result.error = "Track is busy freezing";
        for (auto& cb : uiShown)
            cb(result);
        return;
    }

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

void uapmd_app::AppModel::hidePluginUI(int32_t instanceId) {
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

bool uapmd_app::TransportController::isPlaying() const {
    return sequencer_ && sequencer_->engine()->isPlaybackActive();
}

void uapmd_app::TransportController::play() {
    if (!appModel_->isAudioEngineEnabled())
        return;
    sequencer_->engine()->startPlayback();
    const bool started = sequencer_->engine()->isPlaybackActive();
    appModel_->timeline().isPlaying = started;
    isPlaying_ = started;
    isPaused_ = false;
}

void uapmd_app::TransportController::stop() {
    isRecording_ = false;
    sequencer_->engine()->stopPlayback();
    appModel_->timeline().isPlaying = false;
    appModel_->timeline().playheadPosition.samples = 0;
    appModel_->timeline().playheadPosition.legacy_beats = 0.0;
    isPlaying_ = false;
    isPaused_ = false;
}

void uapmd_app::TransportController::pause() {
    sequencer_->engine()->pausePlayback();
    appModel_->timeline().isPlaying = false;
    isPaused_ = true;
}

void uapmd_app::TransportController::resume() {
    if (!appModel_->isAudioEngineEnabled())
        return;
    sequencer_->engine()->resumePlayback();
    appModel_->timeline().isPlaying =
        sequencer_->engine()->isPlaybackActive();
    isPaused_ = false;
}

void uapmd_app::TransportController::jump(double positionSeconds) {
    sequencer_->engine()->jumpPlayback(positionSeconds);
}

void uapmd_app::TransportController::record() {
    isRecording_ = !isRecording_;
}

std::vector<uapmd_app::AppModel::DeviceEntry> uapmd_app::AppModel::getDevices() const {
    std::lock_guard lock(devicesMutex_);
    return devices_;  // Return copy
}

std::optional<std::shared_ptr<uapmd_app::AppModel::DeviceState>> uapmd_app::AppModel::getDeviceForInstance(int32_t instanceId) const {
    std::lock_guard lock(devicesMutex_);
    for (const auto& entry : devices_) {
        auto state = entry.state;
        if (state && state->pluginInstances.count(instanceId) > 0) {
            return state;
        }
    }
    return std::nullopt;
}

void uapmd_app::AppModel::updateDeviceLabel(int32_t instanceId, const std::string& label) {
    bool updated = false;
    {
        std::lock_guard lock(devicesMutex_);
        for (auto& entry : devices_) {
            auto state = entry.state;
            if (state) {
                std::lock_guard guard(state->mutex);
                if (state->pluginInstances.count(instanceId) > 0) {
                    state->label = label;
                    updated = true;
                    break;
                }
            }
        }
    }
    if (!updated)
        return;
    markPluginInstanceTrackDirty(instanceId);
}

void uapmd_app::AppModel::loadPluginState(
    int32_t instanceId,
    const std::string& filepath,
    PluginStateCallback callback,
    uapmd::ProjectMutationOrigin origin) {
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

    sequencer_.engine()->timeline().setPluginState(
        instanceId,
        std::move(stateData),
        origin,
        [this, instanceId, callback = std::move(callback), result](
            uapmd::ProjectUndoResult undoResult) mutable {
                            auto completed = result;
                            if (!undoResult.succeeded()) {
                                completed.success = false;
                                completed.error = std::move(undoResult.error);
                                std::cerr << completed.error << std::endl;
                            } else {
                                completed.success = true;
                                markPluginInstanceTrackDirty(instanceId);
                                std::cout << "Plugin state loaded from: " << completed.filepath << std::endl;
                            }
                            if (callback)
                                callback(std::move(completed));
                        });
}

void uapmd_app::AppModel::loadPluginState(
    int32_t instanceId,
    DocumentHandle handle,
    PluginStateCallback callback,
    uapmd::ProjectMutationOrigin origin) {
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
                           [this, instanceId, origin, callback = std::move(callback), result](DocumentIOResult ioResult, std::vector<uint8_t> data) mutable {
                               auto completed = result;
                               if (!ioResult.success) {
                                   completed.success = false;
                                   completed.error = ioResult.error;
                                   std::cerr << completed.error << std::endl;
                                   if (callback)
                                       callback(std::move(completed));
                                   return;
                               }

                               sequencer_.engine()->timeline().setPluginState(
                                   instanceId,
                                   std::move(data),
                                   origin,
                                   [this, instanceId, callback = std::move(callback), completed](
                                       uapmd::ProjectUndoResult undoResult) mutable {
                                                       auto finalResult = completed;
                                                       if (!undoResult.succeeded()) {
                                                           finalResult.success = false;
                                                           finalResult.error = std::move(undoResult.error);
                                                           std::cerr << finalResult.error << std::endl;
                                                       } else {
                                                           finalResult.success = true;
                                                           markPluginInstanceTrackDirty(instanceId);
                                                           std::cout << "Plugin state loaded from: " << finalResult.filepath << std::endl;
                                                       }
                                                       if (callback)
                                                           callback(std::move(finalResult));
                                                   });
                           });
}

uapmd_app::AppModel::PluginStateResult uapmd_app::AppModel::loadPluginStateSync(
    int32_t instanceId,
    const std::string& filepath,
    uapmd::ProjectMutationOrigin origin) {
    if (origin == uapmd::ProjectMutationOrigin::Internal) {
        PluginStateResult result;
        result.instanceId = instanceId;
        result.filepath = filepath;
        auto* instance = sequencer_.engine()->getPluginInstance(instanceId);
        if (!instance) {
            result.error = "Failed to get plugin instance";
            return result;
        }
        std::vector<uint8_t> state;
        try {
            std::ifstream file(filepath, std::ios::binary | std::ios::ate);
            if (!file.is_open())
                throw std::runtime_error("Failed to open file for reading");
            const auto fileSize = file.tellg();
            file.seekg(0, std::ios::beg);
            state.resize(static_cast<size_t>(fileSize));
            file.read(
                reinterpret_cast<char*>(state.data()),
                fileSize);
        } catch (const std::exception& ex) {
            result.error = std::format(
                "Failed to load plugin state: {}", ex.what());
            return result;
        }
        auto promise = std::make_shared<std::promise<PluginStateResult>>();
        auto future = promise->get_future();
        instance->loadState(
            std::move(state),
            StateContextType::Project,
            false,
            nullptr,
            [promise, result](std::string error, void*) mutable {
                auto completed = result;
                completed.success = error.empty();
                completed.error = std::move(error);
                promise->set_value(std::move(completed));
            });
        return future.get();
    }
    auto promise = std::make_shared<std::promise<PluginStateResult>>();
    auto future = promise->get_future();
    loadPluginState(instanceId, filepath,
                    [promise](PluginStateResult result) {
                        promise->set_value(std::move(result));
                    },
                    origin);
    return future.get();
}

void uapmd_app::AppModel::savePluginState(int32_t instanceId, const std::string& filepath, PluginStateCallback callback) {
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

    instance->requestState(StateContextType::Project, false, nullptr,
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

void uapmd_app::AppModel::savePluginState(int32_t instanceId, DocumentHandle handle, PluginStateCallback callback) {
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

    instance->requestState(StateContextType::Project, false, nullptr,
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

                               auto* provider = uapmd_app::AppModel::instance().documentProvider();
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

uapmd_app::AppModel::PluginStateResult uapmd_app::AppModel::savePluginStateSync(int32_t instanceId, const std::string& filepath) {
    auto promise = std::make_shared<std::promise<PluginStateResult>>();
    auto future = promise->get_future();
    savePluginState(instanceId, filepath,
                    [promise](PluginStateResult result) {
                        promise->set_value(std::move(result));
                    });
    return future.get();
}

// Timeline and clip management

uapmd_app::AppModel::ClipAddResult uapmd_app::AppModel::addClipToTrack(
    int32_t trackIndex,
    const uapmd::TimelinePosition& position,
    std::unique_ptr<uapmd::AudioFileReader> reader,
    const std::string& filepath
) {
    ClipAddResult result;

    if (filepath.empty() && !reader)
        return addMidiClipToTrack(trackIndex, position, {}, {}, 480, 120.0, {}, {}, "New Clip");

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
    if (result.success)
        markTrackDirty(trackIndex);
    return result;
}

uapmd_app::AppModel::ClipAddResult uapmd_app::AppModel::addMidiClipToTrack(
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

    auto& undo = sequencer_.engine()->timeline().undoEngine();
    const bool ownsCompound = separated.hasMusicalClip()
        && separated.hasMasterTrackClip()
        && !undo.state().compoundOpen;
    if (ownsCompound) {
        auto opened = undo.beginCompound("Import MIDI file");
        if (!opened.succeeded()) {
            result.error = std::move(opened.error);
            return result;
        }
    }

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
        if (!result.success) {
            if (ownsCompound)
                undo.cancelCompound();
            return result;
        }
        markTrackDirty(trackIndex);
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
            if (ownsCompound)
                undo.cancelCompound();
            return result;
        } else {
            markTrackDirty(kMasterTrackIndex);
        }
    }
    if (ownsCompound)
        undo.endCompound();
    return result;
}

uapmd_app::AppModel::ClipAddResult uapmd_app::AppModel::addMidiClipToTrack(
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
    const bool emptyMidiClip = umpEvents.empty() && umpTickTimestamps.empty();
    const int64_t emptyMidiDurationSamples = emptyMidiClip
        ? defaultEmptyMidiClipDurationSamples(sample_rate_, clipTempo, timeSignatureChanges)
        : 0;
    auto& undo = sequencer_.engine()->timeline().undoEngine();
    const bool ownsCompound = emptyMidiClip && !undo.state().compoundOpen;
    if (ownsCompound) {
        auto opened = undo.beginCompound("Add MIDI clip");
        if (!opened.succeeded()) {
            result.error = std::move(opened.error);
            return result;
        }
    }
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
    if (!result.success) {
        if (ownsCompound)
            undo.cancelCompound();
        return result;
    }
    if (emptyMidiClip
        && !sequencer_.engine()->timeline().resizeClip(
            trackIndex, result.clipId, emptyMidiDurationSamples,
            ProjectMutationOrigin::User)) {
        result.success = false;
        result.error = "Could not set the new MIDI clip duration";
        if (ownsCompound)
            undo.cancelCompound();
        return result;
    }
    if (ownsCompound)
        undo.endCompound();
    markTrackDirty(trackIndex);

    return result;
}

uapmd_app::AppModel::ClipAddResult uapmd_app::AppModel::addMasterMidiClip(
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
    if (result.success)
        markTrackDirty(kMasterTrackIndex);
    return result;
}

bool uapmd_app::AppModel::removeClipFromTrack(int32_t trackIndex, int32_t clipId) {
    const bool removed = sequencer_.engine()->timeline().removeClipFromTrack(trackIndex, clipId);
    if (removed)
        markTrackDirty(trackIndex);
    return removed;
}

// ── UMP-level clip editing ────────────────────────────────────────────────────

// Internal helper: look up a MIDI source node, run modifier(words, ticks, error),
// then commit by replacing the clip source node.
namespace {


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

void resolveAllClipAnchorsInAppModel(uapmd_app::AppModel& appModel) {
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
    auto& appModel = uapmd_app::AppModel::instance();
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

    if (!appModel.sequencer().engine()->timeline().replaceMidiClipContent(
            trackIndex, clipId, std::move(newWords), std::move(newTicks))) {
        error = "Failed to replace clip data";
        return false;
    }
    return true;
}

choc::value::Value uapmd_app::AppModel::getMidiClipUmpEvents(int32_t trackIndex, int32_t clipId)
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

bool uapmd_app::AppModel::addUmpEventToClip(int32_t trackIndex, int32_t clipId,
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
    const bool changed = modifyMidiClipUmp(trackIndex, clipId,
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
    if (changed)
        markTrackDirty(trackIndex);
    return changed;
}

bool uapmd_app::AppModel::removeUmpEventFromClip(int32_t trackIndex, int32_t clipId,
                                             int32_t eventIndex, std::string& error)
{
    if (eventIndex < 0) { error = "eventIndex must be >= 0"; return false; }
    const bool changed = modifyMidiClipUmp(trackIndex, clipId,
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
    if (changed)
        markTrackDirty(trackIndex);
    return changed;
}

bool uapmd_app::AppModel::getClipAudioEvents(int32_t trackIndex, int32_t clipId,
                                         std::vector<uapmd::ClipMarker>& markers,
                                         std::vector<uapmd::AudioWarpPoint>& audioWarps,
                                         std::string& error) const
{
    if (trackIndex == uapmd::kMasterTrackIndex) {
        markers = sequencer_.engine()->masterTrackMarkers();
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

bool uapmd_app::AppModel::setMasterTrackMarkersWithValidation(
    std::vector<uapmd::ClipMarker> markers,
    std::string& error,
    uapmd::ProjectMutationOrigin origin)
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

    auto apply = [this](const std::vector<uapmd::ClipMarker>& value) {
        sequencer_.engine()->setMasterTrackMarkers(value);
        resolveAllClipAnchorsInAppModel(*this);
        markTrackDirty(kMasterTrackIndex);
        return true;
    };
    if (origin != ProjectMutationOrigin::User
        && origin != ProjectMutationOrigin::Remote)
        return apply(markers);

    auto operation = std::make_shared<MasterMarkersUndoOperation>(
        sequencer_.engine()->masterTrackMarkers(), std::move(markers), std::move(apply));
    auto result = std::make_shared<std::optional<ProjectUndoResult>>();
    sequencer_.engine()->timeline().undoEngine().perform(
        std::move(operation),
        origin,
        [result](ProjectUndoResult completed) {
            *result = std::move(completed);
        });
    if (result->has_value() && result->value().succeeded())
        return true;
    error = result->has_value() && !result->value().error.empty()
        ? result->value().error
        : "Could not record the master marker edit";
    return false;
}

bool uapmd_app::AppModel::setClipAudioEvents(int32_t trackIndex, int32_t clipId,
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
    auto proposedMasterMarkers = sequencer_.engine()->masterTrackMarkers();
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

    auto reader = uapmd::createAudioFileReaderFromPath(clip->filepath);
    if (!reader) {
        error = "Could not reopen the audio file for warp rebuild.";
        return false;
    }

    auto targetClip = *clip;
    targetClip.markers = markers;
    clipLookup[targetClip.referenceId] = targetClip;
    auto resolvedWarps = resolveAudioWarpPoints(
        targetClip,
        audioWarps,
        clipLookup,
        sequencer_.engine()->masterTrackMarkers(),
                                                static_cast<double>(sampleRate()));
    auto newNode = std::make_unique<uapmd::AudioFileSourceNode>(
        clip->sourceNodeInstanceId,
        std::move(reader),
        static_cast<double>(sampleRate()),
        resolvedWarps
    );

    if (!sequencer_.engine()->timeline().replaceAudioClipContent(
            trackIndex,
            clipId,
            {},
            markers,
            audioWarps,
            sequencer_.engine()->masterTrackMarkers())) {
        error = "Failed to rebuild warped audio source.";
        return false;
    }

    resolveAllClipAnchorsInAppModel(*this);
    markTrackDirty(trackIndex);
    return true;
}

uapmd_app::AppModel::ClipAddResult uapmd_app::AppModel::createEmptyMidiClip(
    int32_t trackIndex, int64_t positionSamples,
    uint32_t tickResolution, double bpm)
{
    uapmd::TimelinePosition pos{};
    pos.samples = positionSamples;
    return addMidiClipToTrack(trackIndex, pos, {}, {}, tickResolution, bpm, {}, {});
}

void uapmd_app::AppModel::importMidiTracksFromFile(
    const std::string& filepath,
    MidiTracksImportCallback callback) {
    if (!callback)
        return;

    struct ImportState {
        uapmd::import::MidiImportResult source;
        MidiTracksImportResult result;
        std::vector<std::string> regularClipReferenceIds;
        MidiTracksImportCallback callback;
        size_t nextTrack{0};
        bool ownsCompound{false};
        bool anchoredAnyMasterClip{false};
    };

    auto state = std::make_shared<ImportState>();
    state->source = uapmd::import::TrackImporter::importMidiFile(filepath);
    state->result.warnings = state->source.warnings;
    state->callback = std::move(callback);
    if (!state->source.success) {
        state->result.error = state->source.error.empty()
            ? "Failed to import MIDI tracks."
            : std::move(state->source.error);
        state->callback(std::move(state->result));
        return;
    }
    state->regularClipReferenceIds.resize(state->source.tracks.size());

    auto& undo = sequencer_.engine()->timeline().undoEngine();
    state->ownsCompound = !undo.state().compoundOpen;
    if (state->ownsCompound) {
        auto opened = undo.beginCompound("Import MIDI tracks");
        if (!opened.succeeded()) {
            state->result.error = std::move(opened.error);
            state->callback(std::move(state->result));
            return;
        }
    }

    auto finish = [this, state]() mutable {
        for (auto& masterTrackClip : state->source.masterTrackClips) {
            uapmd::TimelinePosition position;
            position.samples = 0;
            position.legacy_beats = 0.0;
            auto masterClipResult = addMasterMidiClip(
                position,
                {},
                {},
                masterTrackClip.tickResolution,
                masterTrackClip.detectedTempo,
                std::move(masterTrackClip.tempoChanges),
                std::move(masterTrackClip.timeSignatureChanges),
                masterTrackClip.clipName);
            if (!masterClipResult.success) {
                state->result.warnings.push_back(std::format(
                    "{}: {}", masterTrackClip.clipName, masterClipResult.error));
                continue;
            }

            const int32_t sourceIndex = masterTrackClip.sourceTrackIndex;
            if (sourceIndex < 0
                || sourceIndex >= static_cast<int32_t>(state->regularClipReferenceIds.size())
                || state->regularClipReferenceIds[static_cast<size_t>(sourceIndex)].empty())
                continue;
            if (getMasterTimelineTrack()) {
                sequencer_.engine()->timeline().setClipAnchor(
                    kMasterTrackIndex,
                    masterClipResult.clipId,
                    uapmd::TimeReference::fromContainerStart(
                        state->regularClipReferenceIds[static_cast<size_t>(sourceIndex)], 0.0));
                state->anchoredAnyMasterClip = true;
            }
        }
        if (state->anchoredAnyMasterClip)
            resolveAllClipAnchorsInAppModel(*this);

        state->result.success = !state->result.importedTracks.empty()
            || !state->source.masterTrackClips.empty();
        if (!state->result.success && state->result.error.empty())
            state->result.error = "No MIDI tracks were imported.";
        if (state->ownsCompound)
            sequencer_.engine()->timeline().undoEngine().endCompound();
        state->callback(std::move(state->result));
    };

    auto importNextTrack = [this, state, finish](auto&& self) mutable -> void {
        if (state->nextTrack >= state->source.tracks.size()) {
            finish();
            return;
        }
        const auto sourceIndex = state->nextTrack++;
        addTrack(
            [this, state, finish, self, sourceIndex](
                int32_t newTrackIndex,
                std::string error) mutable {
                auto& track = state->source.tracks[sourceIndex];
                if (newTrackIndex < 0 || !error.empty()) {
                    auto message = error.empty() ? "Failed to create track" : std::move(error);
                    state->result.warnings.push_back(
                        std::format("{}: {}", track.clipName, message));
                    state->result.importedTracks.push_back(
                        {-1, track.clipName, false, std::move(message)});
                    self(self);
                    return;
                }

                uapmd::TimelinePosition position;
                position.samples = 0;
                position.legacy_beats = 0.0;
                auto clipResult = addMidiClipToTrack(
                    newTrackIndex,
                    position,
                    std::move(track.umpEvents),
                    std::move(track.umpTickTimestamps),
                    track.tickResolution,
                    track.detectedTempo,
                    std::move(track.tempoChanges),
                    std::move(track.timeSignatureChanges),
                    track.clipName,
                    track.needsFileSave);
                if (!clipResult.success) {
                    state->result.warnings.push_back(
                        std::format("{}: {}", track.clipName, clipResult.error));
                    state->result.importedTracks.push_back(
                        {newTrackIndex, track.clipName, false, clipResult.error});
                    self(self);
                    return;
                }

                auto tracks = getTimelineTracks();
                if (newTrackIndex >= 0
                    && newTrackIndex < static_cast<int32_t>(tracks.size()))
                    if (auto* createdClip = tracks[newTrackIndex]
                            ->clipManager().getClip(clipResult.clipId))
                        state->regularClipReferenceIds[sourceIndex] = createdClip->referenceId;
                state->result.importedTracks.push_back(
                    {newTrackIndex, track.clipName, true, {}});
                self(self);
            });
    };
    importNextTrack(importNextTrack);
}

uapmd_app::AppModel::MidiTracksImportResult
uapmd_app::AppModel::importMidiTracksFromFileLegacy(const std::string& filepath) {
    MidiTracksImportResult result;
    auto importResult = uapmd::import::TrackImporter::importMidiFile(filepath);
    result.warnings = importResult.warnings;
    if (!importResult.success) {
        result.error = importResult.error.empty()
            ? "Failed to import MIDI tracks."
            : importResult.error;
        return result;
    }

    std::vector<std::string> regularClipReferenceIds(importResult.tracks.size());
    for (size_t index = 0; index < importResult.tracks.size(); ++index) {
        auto& track = importResult.tracks[index];
        const auto newTrackIndex = addTrackLegacy();
        if (newTrackIndex < 0) {
            result.warnings.push_back(
                std::format("{}: Failed to create track", track.clipName));
            result.importedTracks.push_back(
                {-1, track.clipName, false, "Failed to create track"});
            continue;
        }

        uapmd::TimelinePosition position;
        position.samples = 0;
        position.legacy_beats = 0.0;
        auto clipResult = addMidiClipToTrack(
            newTrackIndex,
            position,
            std::move(track.umpEvents),
            std::move(track.umpTickTimestamps),
            track.tickResolution,
            track.detectedTempo,
            std::move(track.tempoChanges),
            std::move(track.timeSignatureChanges),
            track.clipName,
            track.needsFileSave);
        if (!clipResult.success) {
            result.warnings.push_back(
                std::format("{}: {}", track.clipName, clipResult.error));
            result.importedTracks.push_back(
                {newTrackIndex, track.clipName, false, clipResult.error});
            continue;
        }

        auto tracks = getTimelineTracks();
        if (newTrackIndex >= 0
            && newTrackIndex < static_cast<int32_t>(tracks.size()))
            if (auto* createdClip = tracks[newTrackIndex]
                    ->clipManager().getClip(clipResult.clipId))
                regularClipReferenceIds[index] = createdClip->referenceId;
        result.importedTracks.push_back(
            {newTrackIndex, track.clipName, true, {}});
    }

    bool anchoredAnyMasterClip = false;
    for (auto& masterTrackClip : importResult.masterTrackClips) {
        uapmd::TimelinePosition position;
        position.samples = 0;
        position.legacy_beats = 0.0;
        auto masterClipResult = addMasterMidiClip(
            position,
            {},
            {},
            masterTrackClip.tickResolution,
            masterTrackClip.detectedTempo,
            std::move(masterTrackClip.tempoChanges),
            std::move(masterTrackClip.timeSignatureChanges),
            masterTrackClip.clipName);
        if (!masterClipResult.success) {
            result.warnings.push_back(std::format(
                "{}: {}", masterTrackClip.clipName, masterClipResult.error));
            continue;
        }

        const auto sourceIndex = masterTrackClip.sourceTrackIndex;
        if (sourceIndex < 0
            || sourceIndex >= static_cast<int32_t>(regularClipReferenceIds.size())
            || regularClipReferenceIds[static_cast<size_t>(sourceIndex)].empty())
            continue;
        if (auto* masterTrack = getMasterTimelineTrack()) {
            masterTrack->clipManager().setClipAnchor(
                masterClipResult.clipId,
                uapmd::TimeReference::fromContainerStart(
                    regularClipReferenceIds[static_cast<size_t>(sourceIndex)], 0.0),
                static_cast<int32_t>(sampleRate()));
            anchoredAnyMasterClip = true;
        }
    }
    if (anchoredAnyMasterClip)
        resolveAllClipAnchorsInAppModel(*this);

    result.success = !result.importedTracks.empty()
        || !importResult.masterTrackClips.empty();
    if (!result.success && result.error.empty())
        result.error = "No MIDI tracks were imported.";
    return result;
}

int32_t uapmd_app::AppModel::addDeviceInputToTrack(
    int32_t trackIndex,
    const std::vector<uint32_t>& channelIndices
) {
    auto timelineTracks = sequencer_.engine()->timeline().tracks();
    if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(timelineTracks.size()))
        return -1;

    // Device inputs share a track's source-node namespace with clip sources.
    // The engine owns clip allocation, so skip every ID already present before
    // handing one to the mutation layer. This remains valid after project load
    // and undo/redo, where the local counter has no knowledge of engine IDs.
    int32_t sourceNodeId = next_source_node_id_;
    for (;;) {
        const bool inUse = std::any_of(
            timelineTracks.begin(),
            timelineTracks.end(),
            [sourceNodeId](uapmd::TimelineTrack* track) {
                return track && track->getSourceNode(sourceNodeId) != nullptr;
            });
        if (!inUse)
            break;
        ++sourceNodeId;
    }
    next_source_node_id_ = sourceNodeId + 1;
    if (sequencer_.engine()->timeline().addDeviceInputToTrack(
            trackIndex,
            sourceNodeId,
            channelIndices)) {
        markTrackDirty(trackIndex);
        return sourceNodeId;
    }

    return -1;
}

std::vector<uapmd::TimelineTrack*> uapmd_app::AppModel::getTimelineTracks() {
    return sequencer_.engine()->timeline().tracks();
}

uapmd::TimelineTrack* uapmd_app::AppModel::getMasterTimelineTrack() {
    return sequencer_.engine()->timeline().masterTimelineTrack();
}

uapmd_app::AppModel::MasterTrackSnapshot uapmd_app::AppModel::buildMasterTrackSnapshot() {
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

uapmd_app::AppModel::TimelineContentBounds uapmd_app::AppModel::timelineContentBounds() const {
    TimelineContentBounds bounds;
    auto engineBounds = sequencer_.engine()->timeline().calculateContentBounds();
    bounds.hasContent = engineBounds.hasContent;
    bounds.startSeconds = engineBounds.firstSeconds;
    bounds.endSeconds = engineBounds.lastSeconds;
    bounds.durationSeconds = engineBounds.durationSeconds();
    return bounds;
}

void uapmd_app::AppModel::notifyTrackLayoutChanged(const TrackLayoutChange& change) {
    for (auto& cb : trackLayoutChanged) {
        cb(change);
    }
}

int32_t uapmd_app::AppModel::addTrackLegacy() {
    if (!hidden_tracks_.empty()) {
        auto it = hidden_tracks_.begin();
        int32_t reusedIndex = *it;
        hidden_tracks_.erase(it);
        notifyTrackLayoutChanged(TrackLayoutChange{TrackLayoutChange::Type::Added, reusedIndex});
        markProjectDirty();
        return reusedIndex;
    }

    auto trackIndex = sequencer_.engine()->addEmptyTrack();
    if (trackIndex < 0)
        return -1;

    notifyTrackLayoutChanged(TrackLayoutChange{TrackLayoutChange::Type::Added, trackIndex});
    markProjectDirty();
    return trackIndex;
}

void uapmd_app::AppModel::addTrack(TrackMutationCallback callback) {
    if (!callback)
        return;
    sequencer_.engine()->timeline().addEmptyTrack(
        ProjectMutationOrigin::User,
        [this, callback = std::move(callback)](
            int32_t trackIndex,
            std::string error) mutable {
            if (trackIndex < 0 || !error.empty()) {
                callback(-1, std::move(error));
                return;
            }
            notifyTrackLayoutChanged(
                TrackLayoutChange{TrackLayoutChange::Type::Added, trackIndex});
            markProjectDirty();
            callback(trackIndex, {});
        });
}

bool uapmd_app::AppModel::removeTrackLegacy(int32_t trackIndex) {
    auto& uapmdTracks = sequencer_.engine()->tracks();
    if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(uapmdTracks.size()))
        return false;

    auto instances = uapmdTracks[trackIndex]->orderedInstanceIds();
    for (int32_t instanceId : instances) {
        removePluginInstance(instanceId);
    }

    // Clear clips via engine (which owns the timeline tracks)
    sequencer_.engine()->timeline().clearClipsFromTrack(trackIndex);

    hidden_tracks_.insert(trackIndex);
    markProjectDirty();
    markTrackDirty(trackIndex, false);
    notifyTrackLayoutChanged(TrackLayoutChange{TrackLayoutChange::Type::Removed, trackIndex});
    return true;
}

void uapmd_app::AppModel::removeTrack(
    int32_t trackIndex,
    TrackMutationCallback callback) {
    if (!callback)
        return;
    auto& tracks = sequencer_.engine()->tracks();
    if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(tracks.size())) {
        callback(-1, "Invalid track index");
        return;
    }
    if (sequencer_.engine()->frozenTrackManager().isTrackBusy(trackIndex)) {
        callback(-1, "The track is busy rendering");
        return;
    }

    const auto instanceIds = tracks[static_cast<size_t>(trackIndex)]->orderedInstanceIds();
    const bool resumeTransportAfterMutation = pauseTransportForPluginMutation();
    sequencer_.engine()->timeline().removeTrack(
        trackIndex,
        ProjectMutationOrigin::User,
        [this, trackIndex, instanceIds, resumeTransportAfterMutation,
         callback = std::move(callback)](
            int32_t removedIndex,
            std::string error) mutable {
            resumeTransportAfterPluginMutation(resumeTransportAfterMutation);
            if (removedIndex < 0 || !error.empty()) {
                callback(-1, std::move(error));
                return;
            }

            for (const auto instanceId : instanceIds)
                forgetRemovedPluginInstance(instanceId);

            std::set<int32_t> shiftedHiddenTracks;
            for (const auto hiddenIndex : hidden_tracks_) {
                if (hiddenIndex == trackIndex)
                    continue;
                shiftedHiddenTracks.insert(
                    hiddenIndex > trackIndex ? hiddenIndex - 1 : hiddenIndex);
            }
            hidden_tracks_ = std::move(shiftedHiddenTracks);
            {
                std::lock_guard lock(dirtyStateMutex_);
                std::unordered_set<int32_t> shiftedDirtyTracks;
                for (const auto dirtyIndex : dirty_tracks_) {
                    if (dirtyIndex == trackIndex)
                        continue;
                    shiftedDirtyTracks.insert(
                        dirtyIndex > trackIndex ? dirtyIndex - 1 : dirtyIndex);
                }
                dirty_tracks_ = std::move(shiftedDirtyTracks);
            }

            markProjectDirty();
            notifyTrackLayoutChanged(
                TrackLayoutChange{TrackLayoutChange::Type::Removed, trackIndex});
            callback(trackIndex, {});
        });
}

void uapmd_app::AppModel::removeAllTracks(TrackClearCallback callback) {
    if (!callback)
        return;

    struct ClearState {
        TrackClearCallback callback;
        std::unordered_set<int32_t> unaffectedPluginInstanceIds;
        int32_t nextTrackIndex{-1};
        bool ownsCompound{false};
    };
    auto state = std::make_shared<ClearState>();
    state->callback = std::move(callback);
    state->unaffectedPluginInstanceIds = currentPluginInstanceIds();
    state->nextTrackIndex =
        static_cast<int32_t>(sequencer_.engine()->tracks().size()) - 1;

    auto& undo = sequencer_.engine()->timeline().undoEngine();
    state->ownsCompound = !undo.state().compoundOpen;
    if (state->ownsCompound) {
        auto opened = undo.beginCompound("Clear tracks");
        if (!opened.succeeded()) {
            state->callback(std::move(opened.error));
            return;
        }
    }

    auto finish = [this, state]() mutable {
        auto complete = [this, state](uapmd::ProjectUndoResult result) mutable {
            if (!result.succeeded()) {
                state->callback(std::move(result.error));
                return;
            }
            notifyTrackLayoutChanged(
                TrackLayoutChange{TrackLayoutChange::Type::Cleared, -1});
            state->callback({});
        };
        if (state->ownsCompound) {
            sequencer_.engine()->timeline().undoEngine().endCompound(
                std::move(complete));
            return;
        }
        complete(uapmd::ProjectUndoResult::success());
    };

    auto removeNext = [this, state, finish](auto&& self) mutable -> void {
        if (state->nextTrackIndex < 0) {
            finish();
            return;
        }
        const auto trackIndex = state->nextTrackIndex--;
        const auto instanceIds = sequencer_.engine()->tracks()
            [static_cast<size_t>(trackIndex)]->orderedInstanceIds();
        removeTrack(
            trackIndex,
            [this, state, finish, self, instanceIds](
                int32_t removedIndex, std::string error) mutable {
                if (removedIndex < 0 || !error.empty()) {
                    const auto failure = error.empty()
                        ? std::string{"Failed to remove track"}
                        : std::move(error);
                    if (state->ownsCompound) {
                        // Cancelling restores every track already removed; the
                        // completion then republishes the restored app-model state.
                        auto& undo = AppModel::instance().sequencer().engine()
                            ->timeline().undoEngine();
                        undo.cancelCompound(
                            [this, state, failure](uapmd::ProjectUndoResult result) mutable {
                                if (!result.succeeded()) {
                                    state->callback(std::format(
                                        "{}; rollback failed: {}", failure, result.error));
                                    return;
                                }
                                reconcileAfterHistoryMutation(
                                    state->unaffectedPluginInstanceIds);
                                state->callback(std::move(failure));
                            });
                        return;
                    }
                    state->callback(std::move(failure));
                    return;
                }
                for (const auto instanceId : instanceIds)
                    state->unaffectedPluginInstanceIds.erase(instanceId);
                self(self);
            });
    };
    removeNext(removeNext);
}

void uapmd_app::AppModel::removeAllTracksLegacy() {
    auto trackCount = static_cast<int32_t>(sequencer_.engine()->tracks().size());
    for (int32_t i = 0; i < trackCount; ++i)
        removeTrackLegacy(i);
    notifyTrackLayoutChanged(TrackLayoutChange{TrackLayoutChange::Type::Cleared, -1});
}

void uapmd_app::AppModel::saveProjectToDocument(DocumentHandle handle,
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
                [this, provider, handle = std::move(handle), callback = std::move(callback), stage](ProjectResult saveResult) mutable {
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
                                            [this, callback = std::move(callback), stage](DocumentIOResult ioResult) mutable {
                                                if (ioResult.success)
                                                    clearProjectDirtyState();
                                                else
                                                    markProjectDirty();
                                                if (callback)
                                                    callback(ioResult);
                                            });
                });
}

void uapmd_app::AppModel::loadProjectFromResolvedPath(
    const std::filesystem::path& projectFile,
    std::function<void(ProjectResult)> callback)
{
    // Keep the previously loaded archive data alive unless we successfully load a new project.
    std::error_code existsEc;
    if (projectFile.empty() || !std::filesystem::exists(projectFile, existsEc)) {
        callback({false, existsEc ? existsEc.message() : "Project file is unavailable."});
        return;
    }

    if (!ProjectArchive::isArchive(projectFile)) {
        loadProject(projectFile,
            [this, callback = std::move(callback)](ProjectResult result) mutable {
                if (result.success) {
                    if (activeProjectTempDir_)
                        retiredProjectTempDirs_.push_back(std::move(activeProjectTempDir_));
                    for (auto& cb : projectLoaded)
                        cb();
                }
                callback(std::move(result));
            });
        return;
    }

    std::string tempDirError;
    auto tempDir = createTempProjectDirectory(tempDirError);
    if (!tempDir) {
        callback({false, std::move(tempDirError)});
        return;
    }
    auto stage = std::make_unique<ScopedTempDir>(std::move(*tempDir));

    auto extract = ProjectArchive::extractArchive(projectFile, stage->get());
    if (!extract.success) {
        callback({false, std::move(extract.error)});
        return;
    }
    if (extract.projectFile.empty()) {
        callback({false, "Project archive missing .uapmd file."});
        return;
    }

    // Wrap in shared_ptr<unique_ptr> so the lambda is copyable (required by std::function)
    // while ownership of the ScopedTempDir can still be moved into activeProjectTempDir_.
    auto stageHolder = std::make_shared<std::unique_ptr<ScopedTempDir>>(std::move(stage));
    loadProject(extract.projectFile,
        [this, stageHolder, callback = std::move(callback)](ProjectResult result) mutable {
            if (result.success) {
                markLoadedArchiveClipsNeedsFileSave(*this);
                if (activeProjectTempDir_)
                    retiredProjectTempDirs_.push_back(std::move(activeProjectTempDir_));
                activeProjectTempDir_ = std::move(*stageHolder);
                for (auto& cb : projectLoaded)
                    cb();
            }
            callback(std::move(result));
        });
}

uapmd_app::AppModel::ProjectResult uapmd_app::AppModel::loadProjectFromHandleToken(const std::string& token)
{
    ProjectResult failureResult;
    if (token.empty()) {
        failureResult.error = "Project handle token is empty.";
        return failureResult;
    }

    auto* provider = documentProvider();
    if (!provider) {
        failureResult.error = "Document provider unavailable";
        return failureResult;
    }

    auto handle = provider->restoreHandle(token);
    if (!handle || !handle->valid()) {
        failureResult.error = "Project handle token is invalid or no longer accessible.";
        return failureResult;
    }

    auto promise = std::make_shared<std::promise<ProjectResult>>();
    auto future = promise->get_future();
    provider->resolveToPath(*handle,
        [this, promise](DocumentIOResult ioResult, std::filesystem::path path) mutable {
            if (!ioResult.success) {
                promise->set_value(ProjectResult{false, ioResult.error});
                return;
            }
            loadProjectFromResolvedPath(path,
                [promise](ProjectResult result) mutable {
                    promise->set_value(std::move(result));
                });
        });
    return future.get();
}

uapmd_app::AppModel::ProjectResult uapmd_app::AppModel::saveProjectSync(const std::filesystem::path& projectFile) {
    auto promise = std::make_shared<std::promise<ProjectResult>>();
    auto future = promise->get_future();
    saveProject(projectFile,
                [promise](ProjectResult result) {
                    promise->set_value(std::move(result));
                });
    // saveProject requests each plugin's state asynchronously, posting completions back to
    // the main thread; drain the queue while waiting so the save can finish instead of
    // deadlocking against its own main-thread completions.
    while (future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        if (remidy::EventLoop::runningOnMainThread())
            remidy::EventLoop::processQueuedTasks();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return future.get();
}

void uapmd_app::AppModel::saveProject(const std::filesystem::path& projectFile, ProjectSaveCallback callback) {
    auto* engine = sequencer_.engine();
    if (!engine) {
        if (callback)
            callback(ProjectResult{false, "Sequencer engine unavailable"});
        return;
    }

    TimelineFacade::ProjectSaveOptions options;
    options.excludedTrackIndexes.assign(hidden_tracks_.begin(), hidden_tracks_.end());
    engine->timeline().saveProject(
        projectFile,
        std::move(options),
        [this, callback = std::move(callback)](TimelineFacade::ProjectResult result) mutable {
            if (result.success)
                clearProjectDirtyState();
            if (callback)
                callback(ProjectResult{result.success, std::move(result.error)});
        });
}

void uapmd_app::AppModel::loadProject(const std::filesystem::path& projectFile, std::function<void(ProjectResult)> callback) {
    // Validate before replacing the current project: removePluginInstance() is
    // destructive, while TimelineFacade::loadProject() otherwise performs this
    // validation internally before touching the existing timeline.
    if (!UapmdProjectDataReader::read(projectFile)) {
        callback({false, "Failed to parse project file"});
        return;
    }

    // Explicitly tear down every existing instance before replacing the timeline.
    // This destroys each plugin UI while its ContainerWindow is still alive, then
    // emits instanceRemoved so UI subscribers may safely release that container.
    project_load_in_progress_ = true;
    std::unordered_set<int32_t> removedInstanceIds;
    if (auto* mt = sequencer_.engine()->masterTrack()) {
        auto ids = mt->orderedInstanceIds();
        for (int32_t instanceId : ids) {
            if (removedInstanceIds.insert(instanceId).second)
                removePluginInstance(instanceId);
        }
    }

    // Snapshot before removing, since removePluginInstance() erases host entries.
    // pluginHost() may be null in RemoteEngineProxy mode.
    if (auto* host = sequencer_.engine()->pluginHost()) {
        auto ids = host->instanceIds();
        for (int32_t instanceId : ids) {
            if (removedInstanceIds.insert(instanceId).second)
                removePluginInstance(instanceId);
        }
    }
    project_load_in_progress_ = false;

    // Delegate project loading to SequencerEngine asynchronously.
    sequencer_.engine()->timeline().loadProject(projectFile,
        [this, callback = std::move(callback)](TimelineFacade::ProjectResult engineResult) mutable {
            if (!engineResult.success) {
                callback({false, std::move(engineResult.error)});
                return;
            }

            clearProjectDirtyState();

            // Notify UI about all tracks that were created
            hidden_tracks_.clear();
            notifyTrackLayoutChanged(TrackLayoutChange{TrackLayoutChange::Type::Cleared, -1});
            auto numTracks = static_cast<int32_t>(sequencer_.engine()->tracks().size());
            for (int32_t i = 0; i < numTracks; ++i)
                notifyTrackLayoutChanged(TrackLayoutChange{TrackLayoutChange::Type::Added, i});

            // Rebuild device entries and notify listeners for each plugin instance
            clearDeviceEntries();
            if (auto* fbm = sequencer().engine()->functionBlockManager())
                fbm->deleteEmptyDevices();

            if (auto* host = sequencer_.engine()->pluginHost()) {
                for (int32_t instanceId : host->instanceIds()) {
                    auto result = registerPluginInstanceInternal(instanceId, std::nullopt);
                    if (!result.error.empty()) {
                        std::cerr << "Failed to register plugin instance " << instanceId
                                  << ": " << result.error << std::endl;
                    }
                    for (auto& cb : instanceCreated)
                        cb(result);
                }
            }

            callback({true, {}});
        });
}

bool uapmd_app::AppModel::startRenderToFile(const RenderToFileSettings& settings) {
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

void uapmd_app::AppModel::cancelRenderToFile() {
    std::shared_ptr<RenderJobState> job;
    {
        std::scoped_lock jobLock(renderJobMutex_);
        job = activeRenderJob_;
    }
    if (job)
        job->cancel.store(true, std::memory_order_release);
}

uapmd_app::AppModel::RenderToFileStatus uapmd_app::AppModel::getRenderToFileStatus() const {
    std::scoped_lock statusLock(renderStatusMutex_);
    return renderStatus_;
}

void uapmd_app::AppModel::clearCompletedRenderStatus() {
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

void uapmd_app::AppModel::runRenderToFile(RenderToFileSettings settings, std::shared_ptr<RenderJobState> job) {
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
        renderSettings.infiniteTailPolicy = settings.infiniteTailPolicy;
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
std::string uapmd_app::AppModel::generateScanReport() {
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
