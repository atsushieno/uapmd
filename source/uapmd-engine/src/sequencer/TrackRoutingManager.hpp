#pragma once

#include <atomic>
#include <memory>
#include <vector>

#include "uapmd-engine/uapmd-engine.hpp"

namespace uapmd {
    class LatencyCompensationManager;

    class TrackRoutingManager {
        struct TrackRoutingCache {
            std::vector<TrackOutputRoutingTarget> effective_targets{};
            std::vector<uint32_t> path_latencies_in_samples{};
            std::vector<uint32_t> output_alignment_holdbacks_in_samples{};
            uint32_t audible_render_lead_in_samples{0};
        };

        size_t& audio_buffer_size_in_frames_;
        int32_t& sample_rate_;
        uint32_t& default_output_channels_;
        std::vector<std::unique_ptr<SequencerTrack>>& tracks_;
        std::unique_ptr<SequencerTrack>& master_track_;
        std::unique_ptr<AudioProcessContext>& master_track_context_;
        std::unique_ptr<AudioProcessContext>& mix_bus_context_;
        SequenceProcessContext& sequence_;
        TimelineFacade* timeline_{};
        LatencyCompensationManager& latency_compensation_manager_;
        std::vector<TrackRoutingCache> track_routing_caches_{};
        uint32_t max_track_render_lead_in_samples_{0};
        uint32_t max_monitored_live_input_render_lead_in_samples_{0};
        uint32_t max_output_alignment_holdback_in_samples_{0};
        bool output_alignment_active_{false};

        TrackOutputRoutingTarget authoredTrackOutputBusRoutingTarget(
            uapmd_track_index_t trackIndex,
            uint32_t outputBusIndex) const;
        TrackOutputRoutingTarget resolveEffectiveTrackOutputBusRoutingTargetUncached(
            uapmd_track_index_t trackIndex,
            uint32_t outputBusIndex) const;
        TrackOutputRoutingTarget trackOutputBusRoutingTargetImpl(
            uapmd_track_index_t trackIndex,
            uint32_t outputBusIndex) const;
        uint32_t downstreamLatencyInSamplesForTarget(const TrackOutputRoutingTarget& target) const;
        double downstreamTailLengthInSecondsForTarget(const TrackOutputRoutingTarget& target) const;
        double tailLengthSecondsToSamples(double seconds) const;
        uint32_t cachedTrackOutputBusPathLatencyInSamples(
            uapmd_track_index_t trackIndex,
            uint32_t outputBusIndex) const;
        uint32_t fallbackTrackOutputBusPathLatencyInSamples(
            uapmd_track_index_t trackIndex,
            uint32_t outputBusIndex) const;
        uint32_t fallbackTrackAudibleRenderLeadInSamples(uapmd_track_index_t trackIndex) const;
        uint32_t fallbackTrackOutputAlignmentHoldbackInSamples(
            uapmd_track_index_t trackIndex,
            uint32_t outputBusIndex) const;

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
            LatencyCompensationManager& latencyCompensationManager);

        void rebuildRoutingCaches();
        OutputRoutingExtension* outputRoutingExtensionForTrackIndex(uapmd_track_index_t trackIndex) const;
        std::vector<TrackOutputRoutingRule> trackOutputRoutingRules(uapmd_track_index_t trackIndex) const;
        void setTrackOutputRoutingRules(
            uapmd_track_index_t trackIndex,
            const std::vector<TrackOutputRoutingRule>& rules);
        TrackOutputRoutingTarget effectiveTrackOutputBusRoutingTarget(
            uapmd_track_index_t trackIndex,
            uint32_t outputBusIndex) const;
        bool trackUsesLowLatencyMonitoring(uapmd_track_index_t trackIndex) const;
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
