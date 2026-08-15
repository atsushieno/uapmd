#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
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
    , playback_active_(engine.isPlaybackActive())
    , async_lifetime_(std::make_shared<AsyncLifetime>())
    , project_serialization_extension_(
          std::make_unique<FrozenTrackManagerProjectSerializationExtension>(*this))
    , audio_processor_extension_(
          std::make_unique<FrozenTrackAudioProcessorExtension>(*this)) {
    async_lifetime_->owner.store(this, std::memory_order_release);
    const std::weak_ptr<AsyncLifetime> weakLifetime(async_lifetime_);
    transport_quiet_listener_token_ =
        engine_.tailProcessManager().addTransportQuietListener([weakLifetime] {
            remidy::EventLoop::enqueueTaskOnMainThread([weakLifetime] {
                const auto lifetime = weakLifetime.lock();
                auto* owner = lifetime
                    ? lifetime->owner.load(std::memory_order_acquire)
                    : nullptr;
                if (owner)
                    owner->transportBecameQuiet();
            });
        });
    project_document_event_listener_token_ =
        timeline_.projectDocumentEvents().addProjectDocumentEventListener(*this);
    publishPlaybackSnapshot();
}

FrozenTrackManager::~FrozenTrackManager() {
    stopping_.store(true, std::memory_order_release);
    async_lifetime_->owner.store(nullptr, std::memory_order_release);
    if (transport_quiet_listener_token_ != 0)
        engine_.tailProcessManager().removeTransportQuietListener(
            transport_quiet_listener_token_);
    active_playback_snapshot_.store(nullptr, std::memory_order_release);
    if (project_document_event_listener_token_ != 0)
        timeline_.projectDocumentEvents().removeProjectDocumentEventListener(
            project_document_event_listener_token_);

    if (active_render_)
        engine_.finishOfflineTrackRender(true);
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
            runtime.render_deferred_until_transport_quiet = false;
            resetLiveGraph = runtime.state == RuntimeState::Frozen;
            const bool activeRender =
                active_render_ &&
                active_render_->track_reference_id == referenceId;
            if (!resetLiveGraph &&
                (!activeRender ||
                 runtime.state != RuntimeState::Rendering)) {
                runtime.state = RuntimeState::Live;
                playbackState->cached_audio.store(
                    nullptr, std::memory_order_release);
            }
        } else {
            policies_by_track_reference_[referenceId] = FreezePolicy::On;
            if (playback_active_.load(std::memory_order_acquire) ||
                !engine_.tailProcessManager().isTransportQuiet()) {
                runtime.render_deferred_until_transport_quiet = true;
                runtime.state = RuntimeState::Live;
            } else {
                runtime.render_deferred_until_transport_quiet = false;
                runtime.state = RuntimeState::Rendering;
                startRender = true;
            }
        }
    }
    if (resetLiveGraph) {
        // Keep serving the immutable cache while the main-thread reset clears
        // suspended plugin DSP and host alignment buffers. Publish the live
        // state before audio processing is admitted again.
        engine_.resetTrackProcessingState(
            trackIndex,
            true,
            [this, &referenceId, transitionGeneration] {
                std::lock_guard lock(mutex_);
                auto runtime =
                    runtime_by_track_reference_.find(referenceId);
                if (runtime == runtime_by_track_reference_.end() ||
                    runtime->second.invalidation_generation !=
                        transitionGeneration)
                    return;
                runtime->second.state = RuntimeState::Live;
                auto& state =
                    playback_states_by_track_reference_[referenceId];
                state->cached_audio.store(
                    nullptr, std::memory_order_release);
                state->policy.store(
                    FreezePolicy::Off, std::memory_order_release);
                state->runtime_state.store(
                    RuntimeState::Live, std::memory_order_release);
            });
    }
    updatePlaybackState(referenceId);
    if (startRender)
        beginRender(referenceId);
    return true;
}

bool FrozenTrackManager::unfreezeTrack(int32_t trackIndex) {
    return setFreezePolicyForTrack(trackIndex, FreezePolicy::Off);
}

void FrozenTrackManager::transportPlaybackStarted() {
    playback_active_.store(true, std::memory_order_release);
    std::lock_guard lock(mutex_);
    for (auto& [referenceId, runtime] :
         runtime_by_track_reference_) {
        if (runtime.state != RuntimeState::Rendering ||
            !policies_by_track_reference_.contains(referenceId))
            continue;
        ++runtime.invalidation_generation;
        runtime.state = RuntimeState::Live;
        runtime.render_deferred_until_transport_quiet = true;
        auto& state = playback_states_by_track_reference_[referenceId];
        if (!state)
            state = std::make_unique<PlaybackState>();
        state->policy.store(
            FreezePolicy::On, std::memory_order_release);
        state->runtime_state.store(
            RuntimeState::Live, std::memory_order_release);
        state->cached_audio.store(nullptr, std::memory_order_release);
    }
}

void FrozenTrackManager::transportPlaybackStopped() {
    playback_active_.store(false, std::memory_order_release);
    // Play/Resume may have been deferred while an active render restores the
    // plugin instances. Stop/Pause revokes that deferred user request: once
    // the renderer completes, it may resume queued freezing work, but must
    // never change the transport state itself.
    std::lock_guard lock(mutex_);
    pending_playback_start_ = {};
}

void FrozenTrackManager::transportBecameQuiet() {
    if (stopping_.load(std::memory_order_acquire) ||
        playback_active_.load(std::memory_order_acquire) ||
        !engine_.tailProcessManager().isTransportQuiet())
        return;
    std::vector<std::string> deferred;
    {
        std::lock_guard lock(mutex_);
        for (auto& [referenceId, runtime] :
             runtime_by_track_reference_) {
            if (!runtime.render_deferred_until_transport_quiet ||
                !policies_by_track_reference_.contains(referenceId))
                continue;
            runtime.render_deferred_until_transport_quiet = false;
            deferred.push_back(referenceId);
        }
    }
    for (const auto& referenceId : deferred)
        invalidateTrack(referenceId);
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

std::optional<OfflineRenderProgress>
FrozenTrackManager::renderProgressForTrack(int32_t trackIndex) const {
    const auto referenceId = trackReferenceIdForIndex(trackIndex);
    if (referenceId.empty())
        return std::nullopt;
    std::lock_guard lock(mutex_);
    if (!active_render_ ||
        active_render_->track_reference_id != referenceId)
        return std::nullopt;
    return active_render_->progress;
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

bool FrozenTrackManager::isTrackBusy(int32_t trackIndex) const {
    return runtimeStateForTrack(trackIndex) == RuntimeState::Rendering;
}

bool FrozenTrackManager::isInstanceBusy(int32_t instanceId) const {
    const auto trackIndex = engine_.findTrackIndexForInstance(instanceId);
    return trackIndex >= 0 && isTrackBusy(trackIndex);
}

bool FrozenTrackManager::hasBusyTrack() const {
    std::lock_guard lock(mutex_);
    return std::ranges::any_of(
        runtime_by_track_reference_,
        [](const auto& entry) {
            return entry.second.state == RuntimeState::Rendering;
        });
}

void FrozenTrackManager::projectTrackBecameDirty(int32_t trackIndex) {
    if (trackIndex == kMasterTrackIndex) {
        invalidateAllTracks();
        return;
    }
    audioContentChanged(trackIndex);
}

bool FrozenTrackManager::requestPlaybackAfterBusyTrackRestored(
    std::function<void()> startPlayback) {
    std::lock_guard lock(mutex_);
    bool hasActiveRender = false;
    for (auto& [referenceId, runtime] : runtime_by_track_reference_) {
        if (runtime.state != RuntimeState::Rendering ||
            !policies_by_track_reference_.contains(referenceId))
            continue;
        ++runtime.invalidation_generation;
        runtime.render_deferred_until_transport_quiet = true;
        if (active_render_ &&
            active_render_->track_reference_id == referenceId) {
            hasActiveRender = true;
            continue;
        }
        runtime.state = RuntimeState::Live;
        auto& state = playback_states_by_track_reference_[referenceId];
        if (!state)
            state = std::make_unique<PlaybackState>();
        state->policy.store(FreezePolicy::On, std::memory_order_release);
        state->runtime_state.store(
            RuntimeState::Live, std::memory_order_release);
        state->cached_audio.store(nullptr, std::memory_order_release);
    }
    queued_renders_.clear();
    if (!hasActiveRender)
        return false;

    // Playback was explicitly requested while the existing instances were in
    // use by the renderer. Keep the request only until cancellation restores
    // those instances; a successful freeze completion never creates this
    // callback on its own.
    pending_playback_start_ = std::move(startPlayback);
    return true;
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
        policies_by_track_reference_.erase(*event.trackId());
        runtime_by_track_reference_[*event.trackId()].state =
            RuntimeState::Live;
        auto& state = playback_states_by_track_reference_[*event.trackId()];
        if (!state)
            state = std::make_unique<PlaybackState>();
        state->policy.store(FreezePolicy::Off, std::memory_order_release);
        state->runtime_state.store(
            RuntimeState::Live, std::memory_order_release);
        state->cached_audio.store(nullptr, std::memory_order_release);
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
    // PlaybackState objects are intentionally retained for the manager's
    // lifetime. Older realtime snapshots may still contain their addresses.
    publishPlaybackSnapshot();
}

void FrozenTrackManager::trackChanged(const ProjectDocumentEvent& event) {
    // The policy transition itself creates or revokes the frozen render. It is
    // not source material that should invalidate the render it just started.
    if (event.type() == "track-freeze-policy-changed")
        return;
    invalidateTrackForDocumentEvent(event);
}

// A frozen render is a recording of what the track produced at one moment. Any
// change to the track's clips makes it wrong, so it is revoked here rather than
// relying on each call site to remember. Revoking is idempotent -- it bumps a
// monotonic generation counter -- so the existing hand-written
// AppModel::markTrackDirty calls remain harmless.
//
// Plugin graph changes are deliberately not handled the same way: freezing
// manipulates the track's graph, so invalidating on graph changes risks a
// render that revokes itself.
void FrozenTrackManager::invalidateTrackForDocumentEvent(
    const ProjectDocumentEvent& event) {
    if (!event.trackId())
        return;
    invalidateTrack(*event.trackId());
}

void FrozenTrackManager::clipAdded(const ProjectDocumentEvent& event) {
    invalidateTrackForDocumentEvent(event);
}

void FrozenTrackManager::clipRemoved(const ProjectDocumentEvent& event) {
    invalidateTrackForDocumentEvent(event);
}

void FrozenTrackManager::clipChanged(const ProjectDocumentEvent& event) {
    invalidateTrackForDocumentEvent(event);
}

void FrozenTrackManager::audioSourceAdded(const ProjectDocumentEvent&) {
}

void FrozenTrackManager::audioSourceRemoved(const ProjectDocumentEvent&) {
}

void FrozenTrackManager::audioSourceChanged(const ProjectDocumentEvent&) {
}

void FrozenTrackManager::pluginGraphChanged(
    const ProjectDocumentEvent& event) {
    (void) event;
}

void FrozenTrackManager::masterTrackChanged(const ProjectDocumentEvent&) {
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
    if (playback_active_.load(std::memory_order_acquire) ||
        !engine_.tailProcessManager().isTransportQuiet()) {
        std::lock_guard lock(mutex_);
        const std::string referenceId(trackReferenceId);
        auto& runtime = runtime_by_track_reference_[referenceId];
        ++runtime.invalidation_generation;
        runtime.error_message.clear();
        if (!policies_by_track_reference_.contains(referenceId))
            return;
        runtime.render_deferred_until_transport_quiet = true;
        if (runtime.state == RuntimeState::Rendering) {
            runtime.state = RuntimeState::Live;
            auto& state =
                playback_states_by_track_reference_[referenceId];
            if (!state)
                state = std::make_unique<PlaybackState>();
            state->policy.store(
                FreezePolicy::On, std::memory_order_release);
            state->runtime_state.store(
                RuntimeState::Live, std::memory_order_release);
            state->cached_audio.store(
                nullptr, std::memory_order_release);
        }
        return;
    }

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
        const auto transition = [this,
                                 referenceId =
                                     std::string(trackReferenceId),
                                 transitionGeneration,
                                 &rerender] {
            std::lock_guard lock(mutex_);
            auto runtime =
                runtime_by_track_reference_.find(referenceId);
            if (runtime == runtime_by_track_reference_.end() ||
                runtime->second.invalidation_generation !=
                    transitionGeneration) {
                rerender = false;
                return;
            }
            rerender =
                policies_by_track_reference_.contains(referenceId);
            runtime->second.state =
                rerender ? RuntimeState::Rendering : RuntimeState::Live;
            auto& state =
                playback_states_by_track_reference_[referenceId];
            state->cached_audio.store(
                nullptr, std::memory_order_release);
            state->policy.store(
                rerender ? FreezePolicy::On : FreezePolicy::Off,
                std::memory_order_release);
            state->runtime_state.store(
                runtime->second.state, std::memory_order_release);
        };
        if (trackIndex >= 0)
            engine_.resetTrackProcessingState(
                trackIndex, true, transition);
        else
            transition();
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
    const std::weak_ptr<AsyncLifetime> weakLifetime(async_lifetime_);
    remidy::EventLoop::enqueueTaskOnMainThread(
        [weakLifetime, trackReferenceId = std::move(trackReferenceId)]() mutable {
            const auto lifetime = weakLifetime.lock();
            auto* owner = lifetime
                ? lifetime->owner.load(std::memory_order_acquire)
                : nullptr;
            if (owner)
                owner->prepareRender(std::move(trackReferenceId));
        });
}

void FrozenTrackManager::prepareRender(std::string trackReferenceId) {
    if (stopping_.load(std::memory_order_acquire))
        return;
    if (playback_active_.load(std::memory_order_acquire) ||
        !engine_.tailProcessManager().isTransportQuiet()) {
        std::lock_guard lock(mutex_);
        auto runtime =
            runtime_by_track_reference_.find(trackReferenceId);
        if (runtime == runtime_by_track_reference_.end() ||
            !policies_by_track_reference_.contains(trackReferenceId))
            return;
        runtime->second.state = RuntimeState::Live;
        runtime->second.render_deferred_until_transport_quiet = true;
        auto& state =
            playback_states_by_track_reference_[trackReferenceId];
        if (!state)
            state = std::make_unique<PlaybackState>();
        state->policy.store(
            FreezePolicy::On, std::memory_order_release);
        state->runtime_state.store(
            RuntimeState::Live, std::memory_order_release);
        state->cached_audio.store(nullptr, std::memory_order_release);
        return;
    }

    bool queued = false;
    {
        std::lock_guard lock(mutex_);
        if (active_render_) {
            if (std::ranges::find(
                    queued_renders_, trackReferenceId) ==
                queued_renders_.end())
                queued_renders_.push_back(trackReferenceId);
            auto& runtime =
                runtime_by_track_reference_[trackReferenceId];
            runtime.state = RuntimeState::Live;
            queued = true;
        }
    }
    if (queued) {
        updatePlaybackState(trackReferenceId);
        return;
    }

    const auto trackIndex = trackIndexForReferenceId(trackReferenceId);
    if (trackIndex < 0)
        return;

    auto operation = std::make_shared<RenderOperation>();
    operation->track_reference_id = std::move(trackReferenceId);
    operation->settings.trackIndex = trackIndex;
    operation->settings.startSample = 0;
    operation->settings.sampleRate =
        std::max(1, timeline_.state().sample_rate);
    operation->settings.bufferSize = kRenderBufferSize;
    operation->settings.umpBufferSize =
        static_cast<uint32_t>(engine_.umpBufferSizeInBytes());
    operation->settings.maximumBytes = kMaximumFrozenTrackBytes;
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

    const auto bounds = timeline_.calculateTrackContentBounds(trackIndex);
    if (!bounds.hasContent || bounds.lastSample <= 0) {
        failRender(operation, "The track has no renderable timeline content.");
        return;
    }
    auto* track =
        engine_.tracks()[static_cast<size_t>(trackIndex)];
    const auto tailSeconds =
        track ? std::max(0.0, track->tailLengthInSeconds()) : 0.0;
    if (!std::isfinite(tailSeconds)) {
        failRender(
            operation,
            "Tracks with an infinite plugin tail cannot be frozen.");
        return;
    }
    const auto tailSampleCount = std::ceil(
        static_cast<long double>(tailSeconds) *
        static_cast<long double>(operation->settings.sampleRate));
    if (tailSampleCount >
        static_cast<long double>(std::numeric_limits<int64_t>::max())) {
        failRender(operation, "The track tail is too long to render.");
        return;
    }
    const auto tailSamples = static_cast<int64_t>(tailSampleCount);
    const auto latencySamples = track
        ? static_cast<int64_t>(track->latencyInSamples())
        : 0;
    const auto availableAfterContent =
        std::numeric_limits<int64_t>::max() - bounds.lastSample;
    if (latencySamples > availableAfterContent ||
        tailSamples > availableAfterContent - latencySamples) {
        failRender(operation, "The track render range is too long.");
        return;
    }
    operation->settings.endSample =
        bounds.lastSample + tailSamples + latencySamples;
    operation->progress.totalFrames =
        operation->settings.endSample - operation->settings.startSample;
    operation->progress.totalSeconds =
        static_cast<double>(operation->progress.totalFrames) /
        static_cast<double>(operation->settings.sampleRate);

    std::string error;
    if (!engine_.beginOfflineTrackRender(operation->settings, error)) {
        failRender(operation, std::move(error));
        startNextQueuedRender();
        return;
    }
    {
        std::lock_guard lock(mutex_);
        active_render_ = operation;
        auto& runtime =
            runtime_by_track_reference_[operation->track_reference_id];
        runtime.state = RuntimeState::Rendering;
    }
    updatePlaybackState(operation->track_reference_id);
    enqueueRenderStep(operation);
}

void FrozenTrackManager::enqueueRenderStep(
    const std::shared_ptr<RenderOperation>& operation) {
    const std::weak_ptr<AsyncLifetime> weakLifetime(async_lifetime_);
    remidy::EventLoop::enqueueTaskOnMainThread(
        [weakLifetime, operation] {
            const auto lifetime = weakLifetime.lock();
            auto* owner = lifetime
                ? lifetime->owner.load(std::memory_order_acquire)
                : nullptr;
            if (owner)
                owner->renderNextChunk(operation);
        });
}

void FrozenTrackManager::renderNextChunk(
    const std::shared_ptr<RenderOperation>& operation) {
    if (!operationIsCurrent(*operation)) {
        engine_.finishOfflineTrackRender(
            true,
            [this, &operation](OfflineTrackRenderResult&) {
                {
                    std::lock_guard lock(mutex_);
                    auto runtime = runtime_by_track_reference_.find(
                        operation->track_reference_id);
                    if (runtime != runtime_by_track_reference_.end()) {
                        runtime->second.state = RuntimeState::Live;
                        runtime->second.error_message.clear();
                    }
                }
                updatePlaybackState(operation->track_reference_id);
            });
        completeRenderOperation(operation);
        return;
    }

    constexpr uint32_t kBlocksPerEventLoopTurn = 8;
    auto step =
        engine_.renderOfflineTrackStep(kBlocksPerEventLoopTurn);
    {
        std::lock_guard lock(mutex_);
        if (active_render_ == operation)
            operation->progress = step.progress;
    }
    if (step.state == OfflineTrackRenderStepState::InProgress) {
        enqueueRenderStep(operation);
        return;
    }

    if (step.state == OfflineTrackRenderStepState::Error) {
        engine_.finishOfflineTrackRender(
            false,
            [this, &operation, &step](OfflineTrackRenderResult& result) {
                if (result.errorMessage.empty())
                    result.errorMessage = std::move(step.errorMessage);
                failRender(operation, std::move(result.errorMessage));
            });
    } else {
        engine_.finishOfflineTrackRender(
            false,
            [this, &operation](OfflineTrackRenderResult& result) {
                finishRender(operation, std::move(result));
            });
    }
    completeRenderOperation(operation);
}

void FrozenTrackManager::completeRenderOperation(
    const std::shared_ptr<RenderOperation>& operation) {
    std::function<void()> startPlayback;
    {
        std::lock_guard lock(mutex_);
        if (active_render_ == operation)
            active_render_.reset();
        startPlayback = std::move(pending_playback_start_);
    }
    if (startPlayback) {
        // The operation was invalidated by an explicit Play/Resume request.
        // Its state has been restored and its output was not published.
        startPlayback();
        return;
    }
    startNextQueuedRender();
}

void FrozenTrackManager::startNextQueuedRender() {
    while (true) {
        std::string referenceId;
        {
            std::lock_guard lock(mutex_);
            if (active_render_ || queued_renders_.empty())
                return;
            referenceId = std::move(queued_renders_.front());
            queued_renders_.pop_front();
            if (!policies_by_track_reference_.contains(referenceId))
                continue;
        }
        beginRender(std::move(referenceId));
        return;
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

    const auto trackIndex =
        trackIndexForReferenceId(operation->track_reference_id);
    if (trackIndex < 0)
        return;

    // Clear live graph buffers and publish the immutable cache as one audio-
    // excluded transition. Otherwise a callback can refill an alignment or
    // pump buffer between the clear and the state change.
    engine_.resetTrackProcessingState(
        trackIndex,
        false,
        [this, &operation, &cachedAudio] {
            std::lock_guard lock(mutex_);
            if (playback_active_.load(std::memory_order_acquire))
                return;
            auto runtime = runtime_by_track_reference_.find(
                operation->track_reference_id);
            if (runtime == runtime_by_track_reference_.end() ||
                runtime->second.invalidation_generation !=
                    operation->generation ||
                !policies_by_track_reference_.contains(
                    operation->track_reference_id))
                return;

            auto& playbackState =
                playback_states_by_track_reference_[
                    operation->track_reference_id];
            if (!playbackState)
                playbackState = std::make_unique<PlaybackState>();
            const auto* published = cachedAudio.get();
            retained_cached_audio_.push_back(std::move(cachedAudio));
            playbackState->cached_audio.store(
                published, std::memory_order_release);
            runtime->second.state = RuntimeState::Frozen;
            runtime->second.error_message.clear();
            playbackState->policy.store(
                FreezePolicy::On, std::memory_order_release);
            playbackState->runtime_state.store(
                RuntimeState::Frozen, std::memory_order_release);
        });
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
    if (playback_active_.load(std::memory_order_acquire))
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

} // namespace uapmd
