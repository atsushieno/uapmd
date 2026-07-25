#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <functional>
#include <string_view>
#include <vector>

#include <remidy/remidy.hpp>
#include <uapmd-engine/uapmd-engine.hpp>

namespace uapmd {
namespace {

constexpr std::string_view kManifestHeader = "uapmd-track-freezing-v2";
constexpr std::string_view kLegacyManifestHeader = "uapmd-track-freezing-v1";
constexpr uint32_t kRenderBufferSize = 1024;
constexpr uint64_t kMaximumFrozenTrackBytes = 512ULL * 1024ULL * 1024ULL;

std::string_view trim(std::string_view value) {
    while (!value.empty() &&
           (value.front() == ' ' || value.front() == '\t' ||
            value.front() == '\r'))
        value.remove_prefix(1);
    while (!value.empty() &&
           (value.back() == ' ' || value.back() == '\t' ||
            value.back() == '\r'))
        value.remove_suffix(1);
    return value;
}

} // namespace

FrozenTrackManagerProjectSerializationExtension::
    FrozenTrackManagerProjectSerializationExtension(FrozenTrackManager& manager)
    : manager_(manager) {
}

std::string_view
FrozenTrackManagerProjectSerializationExtension::extensionId() const {
    return manager_.extensionId();
}

bool FrozenTrackManagerProjectSerializationExtension::saveProjectExtensionData(
    ProjectSerializationWriteContext& context,
    std::string& error) {
    return manager_.saveProjectExtensionData(context, error);
}

bool FrozenTrackManagerProjectSerializationExtension::loadProjectExtensionData(
    ProjectSerializationReadContext& context,
    std::string& error) {
    return manager_.loadProjectExtensionData(context, error);
}

FrozenTrackAudioProcessorExtension::FrozenTrackAudioProcessorExtension(
    FrozenTrackManager& manager)
    : manager_(manager) {
}

bool FrozenTrackAudioProcessorExtension::shouldProcessAudio(
    SequencerEngine& engine,
    uapmd_track_index_t trackIndex,
    SequencerTrack& track,
    AudioProcessContext& context) {
    return manager_.shouldProcessAudio(
        engine, trackIndex, track, context);
}

void FrozenTrackAudioProcessorExtension::processAudio(
    SequencerEngine& engine,
    uapmd_track_index_t trackIndex,
    SequencerTrack& track,
    AudioProcessContext& context) {
    manager_.processAudio(engine, trackIndex, track, context);
}

void FrozenTrackAudioProcessorExtension::audioContentChanged(
    SequencerEngine&,
    uapmd_track_index_t trackIndex) {
    manager_.audioContentChanged(trackIndex);
}

FrozenTrackManager::FrozenTrackManager(
    SequencerEngine& engine,
    TimelineFacade& timeline)
    : engine_(engine)
    , timeline_(timeline)
    , async_lifetime_(std::make_shared<AsyncLifetime>())
    , project_serialization_extension_(
          std::make_unique<FrozenTrackManagerProjectSerializationExtension>(*this))
    , audio_processor_extension_(
          std::make_unique<FrozenTrackAudioProcessorExtension>(*this)) {
    async_lifetime_->owner.store(this, std::memory_order_release);
    project_document_event_listener_token_ =
        timeline_.projectDocumentEvents().addProjectDocumentEventListener(*this);
    publishPlaybackSnapshot();
}

FrozenTrackManager::~FrozenTrackManager() {
    stopping_.store(true, std::memory_order_release);
    async_lifetime_->owner.store(nullptr, std::memory_order_release);
    active_playback_snapshot_.store(nullptr, std::memory_order_release);
    if (project_document_event_listener_token_ != 0)
        timeline_.projectDocumentEvents().removeProjectDocumentEventListener(
            project_document_event_listener_token_);

    std::vector<std::thread> threads;
    {
        std::lock_guard lock(mutex_);
        threads = std::move(render_threads_);
    }
    for (auto& thread : threads)
        if (thread.joinable())
            thread.join();
}

FrozenTrackManagerProjectSerializationExtension&
FrozenTrackManager::projectSerializationExtension() {
    return *project_serialization_extension_;
}

FrozenTrackAudioProcessorExtension&
FrozenTrackManager::audioProcessorExtension() {
    return *audio_processor_extension_;
}

std::string_view FrozenTrackManager::extensionId() const {
    return kExtensionId;
}

bool FrozenTrackManager::shouldProcessAudio(
    SequencerEngine&,
    uapmd_track_index_t trackIndex,
    SequencerTrack&,
    AudioProcessContext&) {
    const auto* snapshot =
        active_playback_snapshot_.load(std::memory_order_acquire);
    if (!snapshot || trackIndex < 0 ||
        static_cast<size_t>(trackIndex) >= snapshot->tracks.size())
        return false;
    const auto* state = snapshot->tracks[static_cast<size_t>(trackIndex)];
    return state &&
        state->runtime_state.load(std::memory_order_acquire) ==
            RuntimeState::Frozen &&
        state->cached_audio.load(std::memory_order_acquire);
}

void FrozenTrackManager::processAudio(
    SequencerEngine& engine,
    uapmd_track_index_t trackIndex,
    SequencerTrack&,
    AudioProcessContext& context) {
    // The extension owns the entire block when shouldProcessAudio() succeeds.
    // Clear first so a short cache, a missing channel, pause, or stop can never
    // expose output left by the previous callback.
    context.clearAudioOutputs();

    const auto* snapshot =
        active_playback_snapshot_.load(std::memory_order_acquire);
    if (!snapshot || trackIndex < 0 ||
        static_cast<size_t>(trackIndex) >= snapshot->tracks.size() ||
        !engine.isPlaybackActive())
        return;

    const auto* state = snapshot->tracks[static_cast<size_t>(trackIndex)];
    const auto* cachedAudio =
        state ? state->cached_audio.load(std::memory_order_acquire) : nullptr;
    if (!cachedAudio)
        return;

    const int64_t cacheFrame =
        engine.renderPlaybackPosition() - cachedAudio->start_sample;
    const int32_t frameCount = std::max(0, context.frameCount());
    size_t cachedChannel = 0;
    for (int32_t bus = 0; bus < context.audioOutBusCount(); ++bus)
        for (uint32_t channel = 0;
             channel <
             static_cast<uint32_t>(context.outputChannelCount(bus));
             ++channel) {
            auto* output = context.getFloatOutBuffer(bus, channel);
            if (!output || cachedChannel >= cachedAudio->channels.size()) {
                ++cachedChannel;
                continue;
            }
            const auto& input = cachedAudio->channels[cachedChannel++];
            for (int32_t frame = 0; frame < frameCount; ++frame) {
                const int64_t sourceFrame = cacheFrame + frame;
                output[frame] =
                    sourceFrame >= 0 &&
                    static_cast<size_t>(sourceFrame) < input.size()
                    ? input[static_cast<size_t>(sourceFrame)]
                    : 0.0f;
            }
        }
}

void FrozenTrackManager::audioContentChanged(int32_t trackIndex) {
    const auto referenceId = trackReferenceIdForIndex(trackIndex);
    if (!referenceId.empty())
        invalidateTrack(referenceId);
}

FrozenTrackManager::FreezePolicy
FrozenTrackManager::freezePolicyForTrack(int32_t trackIndex) const {
    const auto referenceId = trackReferenceIdForIndex(trackIndex);
    if (referenceId.empty())
        return FreezePolicy::Off;
    std::lock_guard lock(mutex_);
    if (auto it = policies_by_track_reference_.find(referenceId);
        it != policies_by_track_reference_.end())
        return it->second;
    return FreezePolicy::Off;
}

bool FrozenTrackManager::setFreezePolicyForTrack(
    int32_t trackIndex,
    FreezePolicy policy) {
    const auto referenceId = trackReferenceIdForIndex(trackIndex);
    if (referenceId.empty())
        return false;

    bool startRender = false;
    bool resetLiveGraph = false;
    uint64_t transitionGeneration = 0;
    {
        std::lock_guard lock(mutex_);
        const auto existing = policies_by_track_reference_.find(referenceId);
        const auto oldPolicy = existing == policies_by_track_reference_.end()
            ? FreezePolicy::Off
            : existing->second;
        if (oldPolicy == policy)
            return false;

        auto& runtime = runtime_by_track_reference_[referenceId];
        ++runtime.invalidation_generation;
        transitionGeneration = runtime.invalidation_generation;
        runtime.error_message.clear();
        auto& playbackState =
            playback_states_by_track_reference_[referenceId];
        if (!playbackState)
            playbackState = std::make_unique<PlaybackState>();

        if (policy == FreezePolicy::Off) {
            policies_by_track_reference_.erase(referenceId);
            resetLiveGraph = runtime.state == RuntimeState::Frozen;
            if (!resetLiveGraph) {
                runtime.state = RuntimeState::Live;
                playbackState->cached_audio.store(
                    nullptr, std::memory_order_release);
            }
        } else {
            policies_by_track_reference_[referenceId] = FreezePolicy::On;
            runtime.state = RuntimeState::Rendering;
            startRender = true;
        }
    }
    if (resetLiveGraph) {
        // Keep serving the immutable cache while the main-thread reset clears
        // suspended plugin DSP and host alignment buffers.
        engine_.resetTrackProcessingState(trackIndex, true);
        std::lock_guard lock(mutex_);
        auto runtime = runtime_by_track_reference_.find(referenceId);
        if (runtime != runtime_by_track_reference_.end() &&
            runtime->second.invalidation_generation ==
                transitionGeneration) {
            runtime->second.state = RuntimeState::Live;
            auto& state = playback_states_by_track_reference_[referenceId];
            state->cached_audio.store(nullptr, std::memory_order_release);
        }
    }
    updatePlaybackState(referenceId);
    if (startRender)
        beginRender(referenceId);
    return true;
}

bool FrozenTrackManager::unfreezeTrack(int32_t trackIndex) {
    return setFreezePolicyForTrack(trackIndex, FreezePolicy::Off);
}

FrozenTrackManager::RuntimeState
FrozenTrackManager::runtimeStateForTrack(int32_t trackIndex) const {
    const auto referenceId = trackReferenceIdForIndex(trackIndex);
    if (referenceId.empty())
        return RuntimeState::Live;
    std::lock_guard lock(mutex_);
    if (auto it = runtime_by_track_reference_.find(referenceId);
        it != runtime_by_track_reference_.end())
        return it->second.state;
    return RuntimeState::Live;
}

uint64_t FrozenTrackManager::invalidationGenerationForTrack(
    int32_t trackIndex) const {
    const auto referenceId = trackReferenceIdForIndex(trackIndex);
    if (referenceId.empty())
        return 0;
    std::lock_guard lock(mutex_);
    if (auto it = runtime_by_track_reference_.find(referenceId);
        it != runtime_by_track_reference_.end())
        return it->second.invalidation_generation;
    return 1;
}

std::string FrozenTrackManager::errorMessageForTrack(int32_t trackIndex) const {
    const auto referenceId = trackReferenceIdForIndex(trackIndex);
    if (referenceId.empty())
        return {};
    std::lock_guard lock(mutex_);
    if (auto it = runtime_by_track_reference_.find(referenceId);
        it != runtime_by_track_reference_.end())
        return it->second.error_message;
    return {};
}

void FrozenTrackManager::projectLoaded(const ProjectDocumentEvent&) {
    clearAllPlaybackCaches();
}

void FrozenTrackManager::projectClosing(const ProjectDocumentEvent&) {
    {
        std::lock_guard lock(mutex_);
        policies_by_track_reference_.clear();
        runtime_by_track_reference_.clear();
        for (auto& [_, state] : playback_states_by_track_reference_) {
            state->policy.store(FreezePolicy::Off, std::memory_order_release);
            state->runtime_state.store(
                RuntimeState::Live, std::memory_order_release);
            state->cached_audio.store(nullptr, std::memory_order_release);
        }
    }
    publishPlaybackSnapshot();
}

void FrozenTrackManager::trackAdded(const ProjectDocumentEvent& event) {
    if (!event.trackId())
        return;
    {
        std::lock_guard lock(mutex_);
        runtime_by_track_reference_[*event.trackId()].state =
            RuntimeState::Live;
    }
    publishPlaybackSnapshot();
}

void FrozenTrackManager::trackRemoved(const ProjectDocumentEvent& event) {
    if (!event.trackId())
        return;
    {
        std::lock_guard lock(mutex_);
        policies_by_track_reference_.erase(*event.trackId());
        runtime_by_track_reference_.erase(*event.trackId());
        if (auto it =
                playback_states_by_track_reference_.find(*event.trackId());
            it != playback_states_by_track_reference_.end()) {
            it->second->policy.store(
                FreezePolicy::Off, std::memory_order_release);
            it->second->runtime_state.store(
                RuntimeState::Live, std::memory_order_release);
            it->second->cached_audio.store(
                nullptr, std::memory_order_release);
        }
    }
    publishPlaybackSnapshot();
}

void FrozenTrackManager::trackChanged(const ProjectDocumentEvent& event) {
    if (event.trackId())
        invalidateTrack(*event.trackId());
    else
        invalidateAllTracks();
}

void FrozenTrackManager::clipAdded(const ProjectDocumentEvent& event) {
    trackChanged(event);
}

void FrozenTrackManager::clipRemoved(const ProjectDocumentEvent& event) {
    trackChanged(event);
}

void FrozenTrackManager::clipChanged(const ProjectDocumentEvent& event) {
    trackChanged(event);
}

void FrozenTrackManager::audioSourceAdded(const ProjectDocumentEvent&) {
    invalidateAllTracks();
}

void FrozenTrackManager::audioSourceRemoved(const ProjectDocumentEvent&) {
    invalidateAllTracks();
}

void FrozenTrackManager::audioSourceChanged(const ProjectDocumentEvent&) {
    invalidateAllTracks();
}

void FrozenTrackManager::pluginGraphChanged(
    const ProjectDocumentEvent& event) {
    trackChanged(event);
}

void FrozenTrackManager::masterTrackChanged(const ProjectDocumentEvent&) {
    invalidateAllTracks();
}

bool FrozenTrackManager::saveProjectExtensionData(
    ProjectSerializationWriteContext& context,
    std::string& error) {
    std::unordered_map<std::string, FreezePolicy> policies;
    {
        std::lock_guard lock(mutex_);
        policies = policies_by_track_reference_;
    }

    std::string manifest(kManifestHeader);
    manifest += "\n";
    for (const auto& [referenceId, policy] : policies)
        if (!referenceId.empty() && policy == FreezePolicy::On)
            manifest += "track." + referenceId + "=on\n";

    return context.writeExtensionFile(
        extensionId(),
        kManifestPath,
        std::vector<uint8_t>(manifest.begin(), manifest.end()),
        error);
}

bool FrozenTrackManager::loadProjectExtensionData(
    ProjectSerializationReadContext& context,
    std::string& error) {
    std::string readError;
    auto bytes =
        context.readExtensionFile(extensionId(), kManifestPath, readError);
    std::unordered_map<std::string, FreezePolicy> policies;
    if (bytes) {
        std::string_view manifest(
            reinterpret_cast<const char*>(bytes->data()), bytes->size());
        const auto newline = manifest.find('\n');
        const auto header = manifest.substr(0, newline);
        if (header != kManifestHeader && header != kLegacyManifestHeader) {
            error = "Unsupported track-freezing manifest version.";
            return false;
        }

        size_t offset =
            newline == std::string_view::npos ? manifest.size() : newline + 1;
        while (offset < manifest.size()) {
            const auto lineEnd = manifest.find('\n', offset);
            const auto line = trim(manifest.substr(offset, lineEnd - offset));
            offset = lineEnd == std::string_view::npos
                ? manifest.size()
                : lineEnd + 1;
            const auto equals = line.find('=');
            if (equals == std::string_view::npos)
                continue;
            const auto key = trim(line.substr(0, equals));
            const auto value = trim(line.substr(equals + 1));
            constexpr std::string_view trackPrefix{"track."};
            if (key.starts_with(trackPrefix) && value == "on") {
                const auto referenceId = key.substr(trackPrefix.size());
                if (!referenceId.empty())
                    policies.emplace(referenceId, FreezePolicy::On);
            }
        }
    }

    std::vector<std::string> toRender;
    {
        std::lock_guard lock(mutex_);
        policies_by_track_reference_ = std::move(policies);
        runtime_by_track_reference_.clear();
        for (const auto& [referenceId, _] : policies_by_track_reference_) {
            auto& runtime = runtime_by_track_reference_[referenceId];
            runtime.state = RuntimeState::Rendering;
            auto& state = playback_states_by_track_reference_[referenceId];
            if (!state)
                state = std::make_unique<PlaybackState>();
            state->cached_audio.store(nullptr, std::memory_order_release);
            toRender.push_back(referenceId);
        }
    }
    clearAllPlaybackCaches();
    error.clear();
    for (const auto& referenceId : toRender)
        beginRender(referenceId);
    return true;
}

std::string FrozenTrackManager::trackReferenceIdForIndex(
    int32_t trackIndex) const {
    if (trackIndex < 0)
        return {};
    auto tracks = timeline_.tracks();
    if (static_cast<size_t>(trackIndex) >= tracks.size() ||
        !tracks[static_cast<size_t>(trackIndex)])
        return {};
    return tracks[static_cast<size_t>(trackIndex)]->referenceId();
}

int32_t FrozenTrackManager::trackIndexForReferenceId(
    std::string_view trackReferenceId) const {
    auto tracks = timeline_.tracks();
    for (size_t index = 0; index < tracks.size(); ++index)
        if (tracks[index] &&
            tracks[index]->referenceId() == trackReferenceId)
            return static_cast<int32_t>(index);
    return -1;
}

void FrozenTrackManager::invalidateTrack(
    std::string_view trackReferenceId) {
    if (trackReferenceId.empty())
        return;
    bool rerender = false;
    bool resetLiveGraph = false;
    uint64_t transitionGeneration = 0;
    {
        std::lock_guard lock(mutex_);
        const std::string referenceId(trackReferenceId);
        auto& runtime = runtime_by_track_reference_[referenceId];
        ++runtime.invalidation_generation;
        transitionGeneration = runtime.invalidation_generation;
        runtime.error_message.clear();
        rerender =
            policies_by_track_reference_.contains(referenceId);
        resetLiveGraph = runtime.state == RuntimeState::Frozen;
        if (!resetLiveGraph)
            runtime.state =
                rerender ? RuntimeState::Rendering : RuntimeState::Live;
        auto& state = playback_states_by_track_reference_[referenceId];
        if (!state)
            state = std::make_unique<PlaybackState>();
        if (!resetLiveGraph)
            state->cached_audio.store(nullptr, std::memory_order_release);
    }
    if (resetLiveGraph) {
        const auto trackIndex =
            trackIndexForReferenceId(trackReferenceId);
        if (trackIndex >= 0)
            engine_.resetTrackProcessingState(trackIndex, true);
        std::lock_guard lock(mutex_);
        auto runtime = runtime_by_track_reference_.find(
            std::string(trackReferenceId));
        if (runtime != runtime_by_track_reference_.end() &&
            runtime->second.invalidation_generation ==
                transitionGeneration) {
            rerender = policies_by_track_reference_.contains(
                std::string(trackReferenceId));
            runtime->second.state =
                rerender ? RuntimeState::Rendering : RuntimeState::Live;
            auto& state = playback_states_by_track_reference_[
                std::string(trackReferenceId)];
            state->cached_audio.store(nullptr, std::memory_order_release);
        } else {
            rerender = false;
        }
    }
    updatePlaybackState(trackReferenceId);
    if (rerender)
        beginRender(std::string(trackReferenceId));
}

void FrozenTrackManager::invalidateAllTracks() {
    std::vector<std::string> referenceIds;
    {
        std::lock_guard lock(mutex_);
        referenceIds.reserve(runtime_by_track_reference_.size());
        for (const auto& [referenceId, _] :
             runtime_by_track_reference_)
            referenceIds.push_back(referenceId);
    }
    for (const auto& referenceId : referenceIds)
        invalidateTrack(referenceId);
}

void FrozenTrackManager::beginRender(std::string trackReferenceId) {
    if (stopping_.load(std::memory_order_acquire))
        return;
    const auto trackIndex = trackIndexForReferenceId(trackReferenceId);
    if (trackIndex < 0)
        return;

    auto operation = std::make_shared<RenderOperation>();
    operation->track_reference_id = std::move(trackReferenceId);
    operation->sample_rate =
        std::max(1, timeline_.state().sample_rate);
    {
        std::lock_guard lock(mutex_);
        auto runtime =
            runtime_by_track_reference_.find(operation->track_reference_id);
        if (runtime == runtime_by_track_reference_.end() ||
            !policies_by_track_reference_.contains(
                operation->track_reference_id))
            return;
        operation->generation = runtime->second.invalidation_generation;
    }

    if (engine_.trackHasLiveInput(trackIndex)) {
        failRender(
            operation,
            "Tracks with device audio input cannot be frozen.");
        return;
    }

    const auto bounds = timeline_.calculateContentBounds();
    if (!bounds.hasContent || bounds.lastSample <= 0) {
        failRender(operation, "The track has no renderable timeline content.");
        return;
    }
    auto* track =
        engine_.tracks()[static_cast<size_t>(trackIndex)];
    const auto tailSamples = track
        ? static_cast<int64_t>(std::ceil(
              std::max(0.0, track->tailLengthInSeconds()) *
              operation->sample_rate))
        : 0;
    const auto latencySamples =
        track ? static_cast<int64_t>(track->latencyInSamples()) : 0;
    operation->end_sample =
        bounds.lastSample + tailSamples + latencySamples;

    std::error_code tempError;
    const auto uniqueId =
        std::chrono::steady_clock::now().time_since_epoch().count();
    operation->directory =
        std::filesystem::temp_directory_path(tempError) /
        "uapmd-track-freezing" /
        std::format(
            "{:016x}-{}",
            std::hash<std::string>{}(operation->track_reference_id),
            uniqueId);
    if (tempError) {
        failRender(operation, tempError.message());
        return;
    }
    std::filesystem::create_directories(operation->directory, tempError);
    if (tempError) {
        failRender(operation, tempError.message());
        return;
    }
    operation->project_file = operation->directory / "snapshot.uapmd";

    TimelineFacade::ProjectSaveOptions options;
    const auto trackCount = engine_.tracks().size();
    for (size_t index = 0; index < trackCount; ++index)
        if (static_cast<int32_t>(index) != trackIndex)
            options.excludedTrackIndexes.push_back(
                static_cast<int32_t>(index));
    options.emitDocumentEvent = false;

    const std::weak_ptr<AsyncLifetime> weakLifetime(async_lifetime_);
    timeline_.saveProject(
        operation->project_file,
        std::move(options),
        [weakLifetime, operation](TimelineFacade::ProjectResult result) mutable {
            const auto lifetime = weakLifetime.lock();
            auto* owner = lifetime
                ? lifetime->owner.load(std::memory_order_acquire)
                : nullptr;
            if (!owner) {
                FrozenTrackManager::removeTemporarySnapshot(
                    operation->directory);
                return;
            }
            owner->isolatedProjectSaved(
                operation, result.success, std::move(result.error));
        });
}

void FrozenTrackManager::isolatedProjectSaved(
    const std::shared_ptr<RenderOperation>& operation,
    bool success,
    std::string error) {
    if (!success) {
        failRender(operation, std::move(error));
        removeTemporarySnapshot(operation->directory);
        return;
    }
    if (!operationIsCurrent(*operation)) {
        removeTemporarySnapshot(operation->directory);
        return;
    }

    auto isolatedEngine = std::shared_ptr<SequencerEngine>(
        SequencerEngine::create(
            operation->sample_rate,
            kRenderBufferSize,
            engine_.umpBufferSizeInBytes(),
            false)
            .release());
    const std::weak_ptr<AsyncLifetime> weakLifetime(async_lifetime_);
    isolatedEngine->timeline().loadProject(
        operation->project_file,
        [weakLifetime, operation, isolatedEngine](
            TimelineFacade::ProjectResult loadResult) mutable {
            const auto lifetime = weakLifetime.lock();
            auto* owner = lifetime
                ? lifetime->owner.load(std::memory_order_acquire)
                : nullptr;
            if (!owner) {
                isolatedEngine.reset();
                FrozenTrackManager::removeTemporarySnapshot(
                    operation->directory);
                return;
            }
            owner->isolatedProjectLoaded(
                operation,
                std::move(isolatedEngine),
                loadResult.success,
                std::move(loadResult.error));
        });
}

void FrozenTrackManager::isolatedProjectLoaded(
    const std::shared_ptr<RenderOperation>& operation,
    std::shared_ptr<SequencerEngine> isolatedEngine,
    bool success,
    std::string error) {
    if (!success) {
        failRender(operation, std::move(error));
        isolatedEngine.reset();
        removeTemporarySnapshot(operation->directory);
        return;
    }
    if (!operationIsCurrent(*operation) || isolatedEngine->tracks().empty()) {
        if (isolatedEngine->tracks().empty() &&
            operationIsCurrent(*operation))
            failRender(operation, "The isolated track snapshot is empty.");
        isolatedEngine.reset();
        removeTemporarySnapshot(operation->directory);
        return;
    }

    const std::weak_ptr<AsyncLifetime> weakLifetime(async_lifetime_);
    std::thread renderThread(
        [weakLifetime, operation, isolatedEngine = std::move(isolatedEngine)]()
            mutable {
            OfflineTrackRenderSettings settings;
            settings.trackIndex = 0;
            settings.startSample = 0;
            settings.endSample = operation->end_sample;
            settings.sampleRate = operation->sample_rate;
            settings.bufferSize = kRenderBufferSize;
            settings.umpBufferSize =
                static_cast<uint32_t>(isolatedEngine->umpBufferSizeInBytes());
            settings.maximumBytes = kMaximumFrozenTrackBytes;

            OfflineRenderCallbacks callbacks;
            callbacks.shouldCancel = [weakLifetime, operation] {
                const auto lifetime = weakLifetime.lock();
                auto* owner = lifetime
                    ? lifetime->owner.load(std::memory_order_acquire)
                    : nullptr;
                return !owner || !owner->operationIsCurrent(*operation);
            };
            auto renderResult =
                isolatedEngine->renderOfflineTrack(settings, callbacks);
            remidy::EventLoop::enqueueTaskOnMainThread(
                [weakLifetime,
                 operation,
                 isolatedEngine = std::move(isolatedEngine),
                 renderResult = std::move(renderResult)]() mutable {
                    // Plugin formats may require destruction on the main thread.
                    isolatedEngine.reset();
                    const auto lifetime = weakLifetime.lock();
                    auto* owner = lifetime
                        ? lifetime->owner.load(std::memory_order_acquire)
                        : nullptr;
                    if (owner)
                        owner->finishRender(
                            operation, std::move(renderResult));
                    removeTemporarySnapshot(operation->directory);
                });
        });
    {
        std::lock_guard lock(mutex_);
        render_threads_.push_back(std::move(renderThread));
    }
}

void FrozenTrackManager::finishRender(
    const std::shared_ptr<RenderOperation>& operation,
    OfflineTrackRenderResult result) {
    if (!result.success) {
        if (!result.canceled)
            failRender(operation, std::move(result.errorMessage));
        return;
    }

    auto cachedAudio = std::make_unique<CachedAudio>();
    cachedAudio->start_sample = result.startSample;
    cachedAudio->bus_channel_counts = std::move(result.busChannelCounts);
    cachedAudio->channels = std::move(result.channels);

    std::lock_guard lock(mutex_);
    auto runtime =
        runtime_by_track_reference_.find(operation->track_reference_id);
    if (runtime == runtime_by_track_reference_.end() ||
        runtime->second.invalidation_generation != operation->generation ||
        !policies_by_track_reference_.contains(
            operation->track_reference_id))
        return;

    auto& playbackState =
        playback_states_by_track_reference_[operation->track_reference_id];
    if (!playbackState)
        playbackState = std::make_unique<PlaybackState>();
    const auto* published = cachedAudio.get();
    retained_cached_audio_.push_back(std::move(cachedAudio));
    playbackState->cached_audio.store(published, std::memory_order_release);
    runtime->second.state = RuntimeState::Frozen;
    runtime->second.error_message.clear();
    playbackState->policy.store(FreezePolicy::On, std::memory_order_release);
    playbackState->runtime_state.store(
        RuntimeState::Frozen, std::memory_order_release);
}

void FrozenTrackManager::failRender(
    const std::shared_ptr<RenderOperation>& operation,
    std::string error) {
    std::lock_guard lock(mutex_);
    auto runtime =
        runtime_by_track_reference_.find(operation->track_reference_id);
    if (runtime == runtime_by_track_reference_.end() ||
        runtime->second.invalidation_generation != operation->generation ||
        !policies_by_track_reference_.contains(
            operation->track_reference_id))
        return;
    runtime->second.state = RuntimeState::Error;
    runtime->second.error_message =
        error.empty() ? "Track rendering failed." : std::move(error);
    auto& state =
        playback_states_by_track_reference_[operation->track_reference_id];
    if (!state)
        state = std::make_unique<PlaybackState>();
    state->cached_audio.store(nullptr, std::memory_order_release);
    state->policy.store(FreezePolicy::On, std::memory_order_release);
    state->runtime_state.store(
        RuntimeState::Error, std::memory_order_release);
}

bool FrozenTrackManager::operationIsCurrent(
    const RenderOperation& operation) const {
    if (stopping_.load(std::memory_order_acquire))
        return false;
    std::lock_guard lock(mutex_);
    const auto runtime =
        runtime_by_track_reference_.find(operation.track_reference_id);
    return runtime != runtime_by_track_reference_.end() &&
        runtime->second.invalidation_generation == operation.generation &&
        policies_by_track_reference_.contains(operation.track_reference_id);
}

void FrozenTrackManager::updatePlaybackState(
    std::string_view trackReferenceId) {
    std::lock_guard lock(mutex_);
    const std::string referenceId(trackReferenceId);
    auto& state = playback_states_by_track_reference_[referenceId];
    if (!state)
        state = std::make_unique<PlaybackState>();
    const auto policy = policies_by_track_reference_.contains(referenceId)
        ? FreezePolicy::On
        : FreezePolicy::Off;
    const auto runtime = runtime_by_track_reference_.contains(referenceId)
        ? runtime_by_track_reference_.at(referenceId).state
        : RuntimeState::Live;
    state->policy.store(policy, std::memory_order_release);
    state->runtime_state.store(runtime, std::memory_order_release);
    if (runtime != RuntimeState::Frozen)
        state->cached_audio.store(nullptr, std::memory_order_release);
}

void FrozenTrackManager::publishPlaybackSnapshot() {
    std::lock_guard lock(mutex_);
    auto snapshot = std::make_unique<PlaybackSnapshot>();
    const auto tracks = timeline_.tracks();
    snapshot->tracks.resize(tracks.size(), nullptr);
    for (size_t index = 0; index < tracks.size(); ++index) {
        if (!tracks[index])
            continue;
        const auto& referenceId = tracks[index]->referenceId();
        auto& state = playback_states_by_track_reference_[referenceId];
        if (!state)
            state = std::make_unique<PlaybackState>();
        snapshot->tracks[index] = state.get();
    }
    const auto* published = snapshot.get();
    playback_snapshots_.push_back(std::move(snapshot));
    active_playback_snapshot_.store(published, std::memory_order_release);
}

void FrozenTrackManager::clearAllPlaybackCaches() {
    {
        std::lock_guard lock(mutex_);
        for (auto& [referenceId, state] :
             playback_states_by_track_reference_) {
            state->cached_audio.store(nullptr, std::memory_order_release);
            const bool enabled =
                policies_by_track_reference_.contains(referenceId);
            state->policy.store(
                enabled ? FreezePolicy::On : FreezePolicy::Off,
                std::memory_order_release);
            state->runtime_state.store(
                enabled ? RuntimeState::Rendering : RuntimeState::Live,
                std::memory_order_release);
        }
    }
    publishPlaybackSnapshot();
}

void FrozenTrackManager::removeTemporarySnapshot(
    const std::filesystem::path& directory) {
    if (directory.empty())
        return;
    std::error_code error;
    std::filesystem::remove_all(directory, error);
}

} // namespace uapmd
