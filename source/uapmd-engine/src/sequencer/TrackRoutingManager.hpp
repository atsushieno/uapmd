#pragma once

#include <atomic>
#include <memory>
#include <vector>

#include "uapmd-engine/uapmd-engine.hpp"

namespace uapmd {

    class TrackRoutingManager {
        size_t& audio_buffer_size_in_frames_;
        int32_t& sample_rate_;
        uint32_t& default_output_channels_;
        std::vector<std::unique_ptr<SequencerTrack>>& tracks_;
        std::unique_ptr<SequencerTrack>& master_track_;
        std::unique_ptr<AudioProcessContext>& master_track_context_;
        std::unique_ptr<AudioProcessContext>& mix_bus_context_;
        SequenceProcessContext& sequence_;
        TimelineFacade* timeline_{};
        std::atomic<OutputAlignmentMonitoringPolicy>& output_alignment_monitoring_policy_;
        std::atomic<RealtimeInfiniteTailPolicy>& realtime_infinite_tail_policy_;

        TrackOutputRoutingTarget authoredTrackOutputBusRoutingTarget(
            uapmd_track_index_t trackIndex,
            uint32_t outputBusIndex) const;
        TrackOutputRoutingTarget trackOutputBusRoutingTargetImpl(
            uapmd_track_index_t trackIndex,
            uint32_t outputBusIndex) const;
        uint32_t downstreamLatencyInSamplesForTarget(const TrackOutputRoutingTarget& target) const;
        double downstreamTailLengthInSecondsForTarget(const TrackOutputRoutingTarget& target) const;
        uint32_t trackOutputBusPathLatencyInSamples(
            uapmd_track_index_t trackIndex,
            uint32_t outputBusIndex) const;
        double tailLengthSecondsToSamples(double seconds) const;

    public:
        TrackRoutingManager(
            size_t& audioBufferSizeInFrames,
            int32_t& sampleRate,
            uint32_t& defaultOutputChannels,
            std::vector<std::unique_ptr<SequencerTrack>>& tracks,
            std::unique_ptr<SequencerTrack>& masterTrack,
            std::unique_ptr<AudioProcessContext>& masterTrackContext,
            std::unique_ptr<AudioProcessContext>& mixBusContext,
            SequenceProcessContext& sequence,
            TimelineFacade* timeline,
            std::atomic<OutputAlignmentMonitoringPolicy>& outputAlignmentMonitoringPolicy,
            std::atomic<RealtimeInfiniteTailPolicy>& realtimeInfiniteTailPolicy);

        OutputRoutingExtension* outputRoutingExtensionForTrackIndex(uapmd_track_index_t trackIndex) const;
        std::vector<TrackOutputRoutingRule> trackOutputRoutingRules(uapmd_track_index_t trackIndex) const;
        void setTrackOutputRoutingRules(
            uapmd_track_index_t trackIndex,
            const std::vector<TrackOutputRoutingRule>& rules);
        TrackOutputRoutingTarget effectiveTrackOutputBusRoutingTarget(
            uapmd_track_index_t trackIndex,
            uint32_t outputBusIndex) const;
        uint32_t trackAudibleRenderLeadInSamples(uapmd_track_index_t trackIndex) const;
        uint32_t maxTrackRenderLeadInSamples() const;
        uint32_t maxLiveInputRenderLeadInSamples() const;
        uint32_t maxOutputAlignmentHoldbackInSamples() const;
        uint32_t trackOutputAlignmentHoldbackInSamples(
            uapmd_track_index_t trackIndex,
            uint32_t outputBusIndex) const;
        bool isOutputAlignmentActive() const;
        int64_t maxStopDrainInSamples() const;
        void reconfigureMasterTrackInputBuses();
        void reconfigureMixBusContext();
    };

} // namespace uapmd
