#include "LatencyCompensationManager.hpp"

#include <algorithm>

namespace uapmd {

    void LatencyCompensationManager::OutputAlignmentDelayLine::reset() {
        write_position = 0;
        for (auto& bus : buses)
            for (auto& channel : bus)
                std::fill(channel.begin(), channel.end(), 0.0f);
    }

    void LatencyCompensationManager::OutputAlignmentDelayLine::configure(
        AudioProcessContext* ctx,
        size_t delayFrames) {
        capacity_frames = std::max<size_t>(1, delayFrames + 1);
        const uint32_t busCount = ctx ? ctx->audioOutBusCount() : 0;
        buses.assign(busCount, {});
        for (uint32_t busIndex = 0; busIndex < busCount; ++busIndex) {
            const uint32_t channelCount = ctx->outputChannelCount(busIndex);
            buses[busIndex].assign(channelCount, std::vector<float>(capacity_frames, 0.0f));
        }
        write_position = 0;
    }

    LatencyCompensationManager::LatencyCompensationManager(
        size_t& audioBufferSizeInFrames,
        std::vector<std::unique_ptr<SequencerTrack>>& tracks,
        std::unique_ptr<SequencerTrack>& masterTrack,
        SequenceProcessContext& sequence,
        TrackRoutingManager& trackRoutingManager,
        std::atomic<int64_t>& playbackPositionSamples,
        std::atomic<int64_t>& renderPlaybackPositionSamples,
        std::atomic<bool>& latencyDrainActive,
        std::atomic<int64_t>& latencyDrainRemainingSamples,
        std::atomic<bool>& resetToStartAfterLatencyDrain)
        : audio_buffer_size_in_frames_(audioBufferSizeInFrames)
        , tracks_(tracks)
        , master_track_(masterTrack)
        , sequence_(sequence)
        , track_routing_manager_(trackRoutingManager)
        , playback_position_samples_(playbackPositionSamples)
        , render_playback_position_samples_(renderPlaybackPositionSamples)
        , latency_drain_active_(latencyDrainActive)
        , latency_drain_remaining_samples_(latencyDrainRemainingSamples)
        , reset_to_start_after_latency_drain_(resetToStartAfterLatencyDrain) {
    }

    int64_t LatencyCompensationManager::alignToQuantum(int64_t samples) const {
        const auto quantum = static_cast<int64_t>(audio_buffer_size_in_frames_ > 0 ? audio_buffer_size_in_frames_ : 1);
        return ((samples + quantum - 1) / quantum) * quantum;
    }

    void LatencyCompensationManager::clearDrainState() {
        latency_drain_active_.store(false, std::memory_order_release);
        latency_drain_remaining_samples_.store(0, std::memory_order_release);
        reset_to_start_after_latency_drain_.store(false, std::memory_order_release);
    }

    uint32_t LatencyCompensationManager::maxRenderLeadInSamples() const {
        return std::max(
            track_routing_manager_.maxTrackRenderLeadInSamples(),
            master_track_ ? master_track_->renderLeadInSamples() : 0);
    }

    int64_t LatencyCompensationManager::maxStopDrainInSamples() const {
        return track_routing_manager_.maxStopDrainInSamples();
    }

    void LatencyCompensationManager::schedulePrerollFromAudiblePosition(int64_t samples) {
        clearDrainState();
        playback_position_samples_.store(samples, std::memory_order_release);
        render_playback_position_samples_.store(
            samples - alignToQuantum(static_cast<int64_t>(maxRenderLeadInSamples())),
            std::memory_order_release);
    }

    void LatencyCompensationManager::reconfigureOutputAlignmentBuffers() {
        output_alignment_delay_lines_.resize(tracks_.size());
        const size_t delayCapacityFrames =
            static_cast<size_t>(track_routing_manager_.maxOutputAlignmentHoldbackInSamples()) +
            audio_buffer_size_in_frames_;
        for (size_t i = 0; i < output_alignment_delay_lines_.size(); ++i) {
            auto* ctx = i < sequence_.tracks.size() ? sequence_.tracks[i] : nullptr;
            output_alignment_delay_lines_[i].configure(ctx, delayCapacityFrames);
        }
    }

    void LatencyCompensationManager::resetOutputAlignmentBuffers() {
        for (auto& delayLine : output_alignment_delay_lines_)
            delayLine.reset();
    }

    void LatencyCompensationManager::applyLatencyCompensationTimingUpdate(bool isPlaybackActive) {
        reconfigureOutputAlignmentBuffers();
        resetOutputAlignmentBuffers();

        const auto audiblePosition = playback_position_samples_.load(std::memory_order_acquire);
        if (isPlaybackActive) {
            schedulePrerollFromAudiblePosition(audiblePosition);
            return;
        }

        clearDrainState();
        render_playback_position_samples_.store(audiblePosition, std::memory_order_release);
    }

    void LatencyCompensationManager::setPlaybackPosition(int64_t samples, bool isPlaybackActive) {
        if (isPlaybackActive) {
            resetOutputAlignmentBuffers();
            schedulePrerollFromAudiblePosition(samples);
            return;
        }

        clearDrainState();
        playback_position_samples_.store(samples, std::memory_order_release);
        render_playback_position_samples_.store(samples, std::memory_order_release);
        resetOutputAlignmentBuffers();
    }

    void LatencyCompensationManager::startPlayback() {
        resetOutputAlignmentBuffers();
        schedulePrerollFromAudiblePosition(0);
    }

    void LatencyCompensationManager::stopPlayback() {
        resetOutputAlignmentBuffers();
        const auto tailSamples = maxStopDrainInSamples();
        if (tailSamples > 0) {
            latency_drain_active_.store(true, std::memory_order_release);
            latency_drain_remaining_samples_.store(alignToQuantum(tailSamples), std::memory_order_release);
            reset_to_start_after_latency_drain_.store(true, std::memory_order_release);
            return;
        }

        playback_position_samples_.store(0, std::memory_order_release);
        render_playback_position_samples_.store(0, std::memory_order_release);
        clearDrainState();
    }

    void LatencyCompensationManager::pausePlayback() {
        clearDrainState();
        render_playback_position_samples_.store(
            playback_position_samples_.load(std::memory_order_acquire),
            std::memory_order_release);
        resetOutputAlignmentBuffers();
    }

    void LatencyCompensationManager::resumePlayback() {
        resetOutputAlignmentBuffers();
        schedulePrerollFromAudiblePosition(playback_position_samples_.load(std::memory_order_acquire));
    }

    void LatencyCompensationManager::updateLatencyDrainState(int32_t frameCount) {
        if (!latency_drain_active_.load(std::memory_order_acquire))
            return;

        const auto remaining =
            latency_drain_remaining_samples_.fetch_sub(frameCount, std::memory_order_acq_rel) - frameCount;
        render_playback_position_samples_.fetch_add(frameCount, std::memory_order_release);
        if (remaining > 0)
            return;

        latency_drain_active_.store(false, std::memory_order_release);
        latency_drain_remaining_samples_.store(0, std::memory_order_release);
        if (reset_to_start_after_latency_drain_.exchange(false, std::memory_order_acq_rel)) {
            playback_position_samples_.store(0, std::memory_order_release);
            render_playback_position_samples_.store(0, std::memory_order_release);
        }
    }

    void LatencyCompensationManager::applyOutputAlignment(
        uapmd_track_index_t trackIndex,
        AudioProcessContext& ctx,
        int32_t trackFrameCount) {
        if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= output_alignment_delay_lines_.size())
            return;

        auto& delayLine = output_alignment_delay_lines_[static_cast<size_t>(trackIndex)];
        if (delayLine.capacity_frames == 0)
            return;

        bool usedDelay = false;
        const size_t startWritePosition = delayLine.write_position;
        for (uint32_t busIndex = 0; busIndex < ctx.audioOutBusCount(); ++busIndex) {
            const uint32_t outputAlignmentDelay =
                track_routing_manager_.trackOutputAlignmentHoldbackInSamples(trackIndex, busIndex);
            if (outputAlignmentDelay == 0 || busIndex >= delayLine.buses.size())
                continue;

            auto& busStorage = delayLine.buses[busIndex];
            const size_t maxDelayFrames = delayLine.capacity_frames > 0 ? delayLine.capacity_frames - 1 : 0;
            const size_t appliedDelayFrames = std::min<size_t>(outputAlignmentDelay, maxDelayFrames);
            const uint32_t numChannels = std::min<uint32_t>(
                ctx.outputChannelCount(busIndex),
                static_cast<uint32_t>(busStorage.size()));
            for (uint32_t ch = 0; ch < numChannels; ++ch) {
                auto& delayChannel = busStorage[ch];
                float* buffer = ctx.getFloatOutBuffer(busIndex, ch);
                if (!buffer)
                    continue;
                size_t writePosition = startWritePosition;
                for (int32_t frame = 0; frame < trackFrameCount; ++frame) {
                    const float inputSample = buffer[frame];
                    delayChannel[writePosition] = inputSample;
                    const size_t readPosition =
                        (writePosition + delayLine.capacity_frames - appliedDelayFrames) %
                        delayLine.capacity_frames;
                    buffer[frame] = delayChannel[readPosition];
                    writePosition = (writePosition + 1) % delayLine.capacity_frames;
                }
            }
            usedDelay = true;
        }

        if (usedDelay)
            delayLine.write_position =
                (startWritePosition + static_cast<size_t>(trackFrameCount)) % delayLine.capacity_frames;
    }

    bool LatencyCompensationManager::latencyDrainActive() const {
        return latency_drain_active_.load(std::memory_order_acquire);
    }

    int64_t LatencyCompensationManager::playbackPosition() const {
        return playback_position_samples_.load(std::memory_order_acquire);
    }

    int64_t LatencyCompensationManager::renderPlaybackPosition() const {
        return render_playback_position_samples_.load(std::memory_order_acquire);
    }

} // namespace uapmd
