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

class FrozenTrackManager final {
public:
    enum class FreezePolicy : uint8_t {
        Off,
        Auto,
        On,
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
    std::string trackReferenceIdForIndex(int32_t trackIndex) const;
    static std::string policyName(FreezePolicy policy);
    static std::optional<FreezePolicy> parsePolicy(std::string_view value);

    TimelineFacade& timeline_;
    mutable std::mutex mutex_;
    int auto_freeze_minutes_{kDefaultAutoFreezeMinutes};
    std::unordered_map<std::string, FreezePolicy> policies_by_track_reference_;
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

private:
    FrozenTrackManager& manager_;
};

} // namespace uapmd
