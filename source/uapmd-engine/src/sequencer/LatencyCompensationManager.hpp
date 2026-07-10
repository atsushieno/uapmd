#pragma once

#include <atomic>
#include <memory>
#include <vector>

#include "uapmd-engine/uapmd-engine.hpp"
#include "TrackRoutingManager.hpp"

namespace uapmd {

    class LatencyCompensationManager {
        struct OutputAlignmentDelayLine {
            std::vector<std::vector<std::vector<float>>> buses;
            size_t write_position{0};
            size_t capacity_frames{0};

            void reset();
            void configure(AudioProcessContext* ctx, size_t delayFrames);
        };

        size_t& audio_buffer_size_in_frames_;
        std::vector<std::unique_ptr<SequencerTrack>>& tracks_;
        std::unique_ptr<SequencerTrack>& master_track_;
        SequenceProcessContext& sequence_;
        TrackRoutingManager& track_routing_manager_;
        std::atomic<int64_t>& playback_position_samples_;
        std::atomic<int64_t>& render_playback_position_samples_;
        std::atomic<bool>& latency_drain_active_;
        std::atomic<int64_t>& latency_drain_remaining_samples_;
        std::atomic<bool>& reset_to_start_after_latency_drain_;
        std::vector<OutputAlignmentDelayLine> output_alignment_delay_lines_{};

        int64_t alignToQuantum(int64_t samples) const;
        void clearDrainState();
        uint32_t maxRenderLeadInSamples() const;
        int64_t maxStopDrainInSamples() const;
        void schedulePrerollFromAudiblePosition(int64_t samples);

    public:
        LatencyCompensationManager(
            size_t& audioBufferSizeInFrames,
            std::vector<std::unique_ptr<SequencerTrack>>& tracks,
            std::unique_ptr<SequencerTrack>& masterTrack,
            SequenceProcessContext& sequence,
            TrackRoutingManager& trackRoutingManager,
            std::atomic<int64_t>& playbackPositionSamples,
            std::atomic<int64_t>& renderPlaybackPositionSamples,
            std::atomic<bool>& latencyDrainActive,
            std::atomic<int64_t>& latencyDrainRemainingSamples,
            std::atomic<bool>& resetToStartAfterLatencyDrain);

        void reconfigureOutputAlignmentBuffers();
        void resetOutputAlignmentBuffers();
        void applyLatencyCompensationTimingUpdate(bool isPlaybackActive);
        void setPlaybackPosition(int64_t samples, bool isPlaybackActive);
        void startPlayback();
        void stopPlayback();
        void pausePlayback();
        void resumePlayback();
        void updateLatencyDrainState(int32_t frameCount);
        void applyOutputAlignment(
            uapmd_track_index_t trackIndex,
            AudioProcessContext& ctx,
            int32_t trackFrameCount);

        bool latencyDrainActive() const;
        int64_t playbackPosition() const;
        int64_t renderPlaybackPosition() const;
    };

} // namespace uapmd
