#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <uapmd/uapmd.hpp>
#include <uapmd-engine/detail/sequencer/LatencyCompensationManager.hpp>

namespace uapmd {
    class TrackRoutingManager;
    class SequenceProcessContext;
    class SequencerTrack;

    class LatencyCompensationManagerImpl final : public LatencyCompensationManager {
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
        TrackRoutingManager* track_routing_manager_{};
        std::atomic<PlaybackCompensationMode> playback_compensation_mode_{
            PlaybackCompensationMode::COMPENSATED};
        std::atomic<InputMonitoringPolicy> input_monitoring_policy_{
            InputMonitoringPolicy::AUTO};
        std::vector<uint8_t> track_record_armed_{};
        std::vector<uint8_t> track_monitoring_enabled_{};
        std::atomic<OutputAlignmentMonitoringPolicy> output_alignment_monitoring_policy_{
            OutputAlignmentMonitoringPolicy::LOW_LATENCY_LIVE_INPUT};
        std::atomic<RealtimeInfiniteTailPolicy> realtime_infinite_tail_policy_{
            RealtimeInfiniteTailPolicy::LATENCY_FALLBACK};
        std::atomic<bool>& is_playback_active_;
        std::atomic<int64_t>& playback_position_samples_;
        std::atomic<int64_t>& render_playback_position_samples_;
        std::atomic<bool>& latency_drain_active_;
        std::atomic<int64_t>& latency_drain_remaining_samples_;
        std::atomic<bool>& reset_to_start_after_latency_drain_;
        std::vector<OutputAlignmentDelayLine> output_alignment_delay_lines_{};
        std::function<void(const std::function<void()>&)> run_mutation_{};

        int64_t alignToQuantum(int64_t samples) const;
        void clearDrainState();
        uint32_t maxRenderLeadInSamples() const;
        int64_t maxStopDrainInSamples() const;
        void schedulePrerollFromAudiblePosition(int64_t samples);
        void applyStateChange();

    public:
        LatencyCompensationManagerImpl(
            size_t& audioBufferSizeInFrames,
            std::vector<std::unique_ptr<SequencerTrack>>& tracks,
            std::unique_ptr<SequencerTrack>& masterTrack,
            SequenceProcessContext& sequence,
            std::atomic<bool>& isPlaybackActive,
            std::atomic<int64_t>& playbackPositionSamples,
            std::atomic<int64_t>& renderPlaybackPositionSamples,
            std::atomic<bool>& latencyDrainActive,
            std::atomic<int64_t>& latencyDrainRemainingSamples,
            std::atomic<bool>& resetToStartAfterLatencyDrain,
            std::function<void(const std::function<void()>&)> runMutation);

        void attachTrackRoutingManager(TrackRoutingManager& trackRoutingManager);
        bool trackRecordArmed(uapmd_track_index_t trackIndex) const override;
        void trackRecordArmed(uapmd_track_index_t trackIndex, bool armed) override;
        bool trackMonitoringEnabled(uapmd_track_index_t trackIndex) const override;
        void trackMonitoringEnabled(uapmd_track_index_t trackIndex, bool enabled) override;
        PlaybackCompensationMode playbackCompensationMode() const override;
        void playbackCompensationMode(PlaybackCompensationMode mode) override;
        InputMonitoringPolicy inputMonitoringPolicy() const override;
        void inputMonitoringPolicy(InputMonitoringPolicy policy) override;
        LatencyCompensationProjectSettings projectSettings() const override;
        bool applyProjectSettings(const LatencyCompensationProjectSettings& settings, std::string& error) override;
        OutputAlignmentMonitoringPolicy outputAlignmentMonitoringPolicy() const override;
        void outputAlignmentMonitoringPolicy(OutputAlignmentMonitoringPolicy policy) override;
        RealtimeInfiniteTailPolicy realtimeInfiniteTailPolicy() const override;
        void realtimeInfiniteTailPolicy(RealtimeInfiniteTailPolicy policy) override;
        void onTrackAdded();
        void onTrackRemoved(uapmd_track_index_t trackIndex);

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
