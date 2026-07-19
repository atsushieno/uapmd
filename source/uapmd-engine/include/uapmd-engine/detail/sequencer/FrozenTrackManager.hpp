#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

#include <uapmd-data/uapmd-data.hpp>

#include "TrackAudioProcessorExtension.hpp"

namespace uapmd {

class TimelineFacade;
class FrozenTrackAudioProcessorExtension;
class FrozenTrackManagerProjectSerializationExtension;

class FrozenTrackManager final : public ProjectDocumentEventListener {
public:
    enum class FreezePolicy : uint8_t {
        Off,
        Auto,
        On,
    };

    enum class RuntimeState : uint8_t {
        Live,
        Waiting,
        Rendering,
        Frozen,
        Unfreezing,
        Invalid,
        Error,
        CachingUnavailable,
    };

    explicit FrozenTrackManager(TimelineFacade& timeline);
    ~FrozenTrackManager();

    FrozenTrackManagerProjectSerializationExtension& projectSerializationExtension();
    FrozenTrackAudioProcessorExtension& audioProcessorExtension();

    int autoFreezeMinutes() const;
    bool setAutoFreezeMinutes(int minutes);

    FreezePolicy freezePolicyForTrack(int32_t trackIndex) const;
    bool setFreezePolicyForTrack(int32_t trackIndex, FreezePolicy policy);
    bool unfreezeTrack(int32_t trackIndex);
    RuntimeState runtimeStateForTrack(int32_t trackIndex) const;
    uint64_t invalidationGenerationForTrack(int32_t trackIndex) const;

private:
    friend class FrozenTrackAudioProcessorExtension;
    friend class FrozenTrackManagerProjectSerializationExtension;

    static constexpr std::string_view kExtensionId{"org.uapmd.app.track-freezing"};
    static constexpr std::string_view kManifestPath{"track-freezing.ini"};
    static constexpr int kDefaultAutoFreezeMinutes = 5;

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
    void invalidateTrack(std::string_view trackReferenceId);
    void invalidateAllTracks();
    RuntimeState stateForPolicy(FreezePolicy policy) const;
    static std::string policyName(FreezePolicy policy);
    static std::optional<FreezePolicy> parsePolicy(std::string_view value);

    TimelineFacade& timeline_;
    mutable std::mutex mutex_;
    int auto_freeze_minutes_{kDefaultAutoFreezeMinutes};
    std::unordered_map<std::string, FreezePolicy> policies_by_track_reference_;
    struct TrackRuntime {
        RuntimeState state{RuntimeState::Live};
        uint64_t invalidation_generation{1};
    };
    std::unordered_map<std::string, TrackRuntime> runtime_by_track_reference_;
    ProjectDocumentEventListenerToken project_document_event_listener_token_{0};
    std::unique_ptr<FrozenTrackManagerProjectSerializationExtension> project_serialization_extension_;
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
