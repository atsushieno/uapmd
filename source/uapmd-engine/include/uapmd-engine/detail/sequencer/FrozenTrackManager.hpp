#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <uapmd-data/uapmd-data.hpp>

#include "OfflineRenderer.hpp"
#include "TrackAudioProcessorExtension.hpp"

namespace uapmd {

class TimelineFacade;
class FrozenTrackAudioProcessorExtension;
class FrozenTrackManagerProjectSerializationExtension;

class FrozenTrackManager final : public ProjectDocumentEventListener {
public:
    enum class FreezePolicy : uint8_t {
        Off,
        On,
    };

    enum class RuntimeState : uint8_t {
        Live,
        Rendering,
        Frozen,
        Error,
    };

    FrozenTrackManager(SequencerEngine& engine, TimelineFacade& timeline);
    ~FrozenTrackManager();

    FrozenTrackManagerProjectSerializationExtension& projectSerializationExtension();
    FrozenTrackAudioProcessorExtension& audioProcessorExtension();

    FreezePolicy freezePolicyForTrack(int32_t trackIndex) const;
    bool setFreezePolicyForTrack(int32_t trackIndex, FreezePolicy policy);
    bool unfreezeTrack(int32_t trackIndex);
    RuntimeState runtimeStateForTrack(int32_t trackIndex) const;
    uint64_t invalidationGenerationForTrack(int32_t trackIndex) const;
    std::string errorMessageForTrack(int32_t trackIndex) const;
    bool isTrackBusy(int32_t trackIndex) const;
    bool isInstanceBusy(int32_t instanceId) const;
    bool hasBusyTrack() const;
    void projectTrackBecameDirty(int32_t trackIndex);
    bool requestPlaybackAfterBusyTrackRestored(
        std::function<void()> startPlayback);
    void transportPlaybackStarted();
    void transportPlaybackStopped();

private:
    friend class FrozenTrackAudioProcessorExtension;
    friend class FrozenTrackManagerProjectSerializationExtension;

    static constexpr std::string_view kExtensionId{"org.uapmd.app.track-freezing"};
    static constexpr std::string_view kManifestPath{"track-freezing.ini"};

    struct CachedAudio {
        int64_t start_sample{0};
        std::vector<uint32_t> bus_channel_counts;
        std::vector<std::vector<float>> channels;
    };

    struct PlaybackState {
        std::atomic<FreezePolicy> policy{FreezePolicy::Off};
        std::atomic<RuntimeState> runtime_state{RuntimeState::Live};
        std::atomic<const CachedAudio*> cached_audio{nullptr};
    };

    struct PlaybackSnapshot {
        std::vector<PlaybackState*> tracks;
    };

    struct TrackRuntime {
        RuntimeState state{RuntimeState::Live};
        uint64_t invalidation_generation{1};
        bool render_deferred_until_transport_quiet{false};
        std::string error_message;
    };

    struct RenderOperation {
        std::string track_reference_id;
        uint64_t generation{0};
        OfflineTrackRenderSettings settings;
    };

    struct AsyncLifetime {
        std::atomic<FrozenTrackManager*> owner{nullptr};
    };

    std::string_view extensionId() const;
    bool saveProjectExtensionData(ProjectSerializationWriteContext& context, std::string& error);
    bool loadProjectExtensionData(ProjectSerializationReadContext& context, std::string& error);
    bool shouldProcessAudio(
        SequencerEngine& engine,
        uapmd_track_index_t trackIndex,
        SequencerTrack& track,
        AudioProcessContext& context);
    void processAudio(
        SequencerEngine& engine,
        uapmd_track_index_t trackIndex,
        SequencerTrack& track,
        AudioProcessContext& context);
    void audioContentChanged(int32_t trackIndex);
    void projectLoaded(const ProjectDocumentEvent& event) override;
    void projectClosing(const ProjectDocumentEvent& event) override;
    void trackAdded(const ProjectDocumentEvent& event) override;
    void trackRemoved(const ProjectDocumentEvent& event) override;
    void trackChanged(const ProjectDocumentEvent& event) override;
    void clipAdded(const ProjectDocumentEvent& event) override;
    void clipRemoved(const ProjectDocumentEvent& event) override;
    void clipChanged(const ProjectDocumentEvent& event) override;
    void audioSourceAdded(const ProjectDocumentEvent& event) override;
    void audioSourceRemoved(const ProjectDocumentEvent& event) override;
    void audioSourceChanged(const ProjectDocumentEvent& event) override;
    void pluginGraphChanged(const ProjectDocumentEvent& event) override;
    void masterTrackChanged(const ProjectDocumentEvent& event) override;

    std::string trackReferenceIdForIndex(int32_t trackIndex) const;
    int32_t trackIndexForReferenceId(std::string_view trackReferenceId) const;
    void invalidateTrack(std::string_view trackReferenceId);
    void invalidateAllTracks();
    void transportBecameQuiet();
    void beginRender(std::string trackReferenceId);
    void prepareRender(std::string trackReferenceId);
    void enqueueRenderStep(const std::shared_ptr<RenderOperation>& operation);
    void renderNextChunk(const std::shared_ptr<RenderOperation>& operation);
    void completeRenderOperation(
        const std::shared_ptr<RenderOperation>& operation);
    void startNextQueuedRender();
    void finishRender(
        const std::shared_ptr<RenderOperation>& operation,
        OfflineTrackRenderResult result);
    void failRender(
        const std::shared_ptr<RenderOperation>& operation,
        std::string error);
    bool operationIsCurrent(const RenderOperation& operation) const;
    void updatePlaybackState(std::string_view trackReferenceId);
    void publishPlaybackSnapshot();
    void clearAllPlaybackCaches();

    SequencerEngine& engine_;
    TimelineFacade& timeline_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, FreezePolicy> policies_by_track_reference_;
    std::unordered_map<std::string, TrackRuntime> runtime_by_track_reference_;
    std::unordered_map<std::string, std::unique_ptr<PlaybackState>>
        playback_states_by_track_reference_;
    std::vector<std::unique_ptr<PlaybackSnapshot>> playback_snapshots_;
    std::atomic<const PlaybackSnapshot*> active_playback_snapshot_{nullptr};
    std::vector<std::unique_ptr<CachedAudio>> retained_cached_audio_;
    std::shared_ptr<RenderOperation> active_render_;
    std::deque<std::string> queued_renders_;
    std::function<void()> pending_playback_start_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> playback_active_{false};
    std::shared_ptr<AsyncLifetime> async_lifetime_;
    uint64_t transport_quiet_listener_token_{0};
    ProjectDocumentEventListenerToken project_document_event_listener_token_{0};
    std::unique_ptr<FrozenTrackManagerProjectSerializationExtension>
        project_serialization_extension_;
    std::unique_ptr<FrozenTrackAudioProcessorExtension> audio_processor_extension_;
};

class FrozenTrackManagerProjectSerializationExtension final : public ProjectSerializationExtension {
public:
    explicit FrozenTrackManagerProjectSerializationExtension(FrozenTrackManager& manager);

    std::string_view extensionId() const override;
    bool saveProjectExtensionData(ProjectSerializationWriteContext& context, std::string& error) override;
    bool loadProjectExtensionData(ProjectSerializationReadContext& context, std::string& error) override;

private:
    FrozenTrackManager& manager_;
};

class FrozenTrackAudioProcessorExtension final : public TrackAudioProcessorExtension {
public:
    explicit FrozenTrackAudioProcessorExtension(FrozenTrackManager& manager);

    bool shouldProcessAudio(
        SequencerEngine& engine,
        uapmd_track_index_t trackIndex,
        SequencerTrack& track,
        AudioProcessContext& context) override;
    void processAudio(
        SequencerEngine& engine,
        uapmd_track_index_t trackIndex,
        SequencerTrack& track,
        AudioProcessContext& context) override;
    void audioContentChanged(SequencerEngine& engine, uapmd_track_index_t trackIndex) override;

private:
    FrozenTrackManager& manager_;
};

} // namespace uapmd
