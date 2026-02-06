#include <algorithm>
#include <cstring>
#include "uapmd-engine/uapmd-engine.hpp"

namespace uapmd {

    AudioFileSourceNode::AudioFileSourceNode(
        int32_t instanceId,
        std::unique_ptr<uapmd::AudioFileReader> reader,
        double targetSampleRate
    ) : instance_id_(instanceId), reader_(std::move(reader)), target_sample_rate_(targetSampleRate) {

        if (!reader_)
            return;

        // Get file properties
        const auto& props = reader_->getProperties();
        channel_count_ = props.numChannels;
        num_frames_ = props.numFrames;
        sample_rate_ = props.sampleRate;

        // Load entire file into memory (no locking needed - constructor runs before playback)
        audio_buffer_.clear();
        audio_buffer_.resize(channel_count_);

        for (uint32_t ch = 0; ch < channel_count_; ++ch) {
            audio_buffer_[ch].resize(num_frames_);
        }

        // Prepare array of channel pointers for planar read
        std::vector<float*> destPtrs;
        destPtrs.reserve(channel_count_);
        for (uint32_t ch = 0; ch < channel_count_; ++ch) {
            destPtrs.push_back(audio_buffer_[ch].data());
        }

        // Read all frames into our planar buffers
        reader_->readFrames(0, num_frames_, destPtrs.data(), channel_count_);

        // Mark buffer as ready for realtime reading (memory barrier ensures visibility)
        buffer_ready_.store(true, std::memory_order_release);
    }

    void AudioFileSourceNode::seek(int64_t samplePosition) {
        playback_position_.store(samplePosition, std::memory_order_release);
        // Update source position (convert from target rate to source rate)
        const double sampleRateTolerance = 0.01;
        if (std::abs(sample_rate_ - target_sample_rate_) > sampleRateTolerance) {
            source_position_.store(samplePosition * (sample_rate_ / target_sample_rate_), std::memory_order_release);
        } else {
            source_position_.store(samplePosition, std::memory_order_release);
        }
    }

    int64_t AudioFileSourceNode::currentPosition() const {
        return playback_position_.load(std::memory_order_acquire);
    }

    int64_t AudioFileSourceNode::totalLength() const {
        // Return length in target sample rate domain
        const double sampleRateTolerance = 0.01;
        if (std::abs(sample_rate_ - target_sample_rate_) > sampleRateTolerance) {
            double ratio = target_sample_rate_ / sample_rate_;
            return static_cast<int64_t>(std::round(num_frames_ * ratio));
        }
        return num_frames_;
    }

    void AudioFileSourceNode::processAudio(float** buffers, uint32_t numChannels, int32_t frameCount) {
        // Clear output buffers first
        for (uint32_t ch = 0; ch < numChannels; ++ch) {
            if (buffers[ch])
                std::memset(buffers[ch], 0, frameCount * sizeof(float));
        }

        if (bypassed_ || !is_playing_.load(std::memory_order_acquire))
            return;

        // Realtime-safe check: is buffer ready? (no blocking)
        if (!buffer_ready_.load(std::memory_order_acquire))
            return;

        if (audio_buffer_.empty())
            return;

        int64_t pos = playback_position_.load(std::memory_order_acquire);

        // Check if resampling is needed
        const double sampleRateTolerance = 0.01;
        bool needsResampling = std::abs(sample_rate_ - target_sample_rate_) > sampleRateTolerance;

        if (!needsResampling) {
            // Fast path: no resampling needed, direct copy
            if (pos < 0 || pos >= num_frames_)
                return;

            int64_t framesToCopy = std::min(static_cast<int64_t>(frameCount), num_frames_ - pos);
            if (framesToCopy <= 0)
                return;

            for (uint32_t ch = 0; ch < std::min(numChannels, channel_count_); ++ch) {
                if (buffers[ch] && ch < audio_buffer_.size()) {
                    const float* src = &audio_buffer_[ch][pos];
                    std::memcpy(buffers[ch], src, framesToCopy * sizeof(float));
                }
            }

            playback_position_.store(pos + framesToCopy, std::memory_order_release);
        } else {
            // Resampling path: use linear interpolation
            double resampleIncrement = sample_rate_ / target_sample_rate_;
            int32_t framesCopied = 0;

            // Load current source position atomically
            double currentSourcePos = source_position_.load(std::memory_order_acquire);

            for (int32_t i = 0; i < frameCount; ++i) {
                // Check if we're past the end of the source buffer
                if (currentSourcePos >= num_frames_ - 1)
                    break;

                int64_t sourceIndex = static_cast<int64_t>(currentSourcePos);
                double fraction = currentSourcePos - sourceIndex;

                // Linear interpolation between adjacent samples
                // Reading from pre-allocated buffer (realtime-safe)
                for (uint32_t ch = 0; ch < std::min(numChannels, channel_count_); ++ch) {
                    if (buffers[ch] && ch < audio_buffer_.size()) {
                        float sample0 = audio_buffer_[ch][sourceIndex];
                        float sample1 = audio_buffer_[ch][sourceIndex + 1];
                        buffers[ch][i] = sample0 + fraction * (sample1 - sample0);
                    }
                }

                currentSourcePos += resampleIncrement;
                framesCopied++;
            }

            // Store updated source position atomically
            source_position_.store(currentSourcePos, std::memory_order_release);
            playback_position_.store(pos + framesCopied, std::memory_order_release);
        }
    }

    std::vector<uint8_t> AudioFileSourceNode::saveState() {
        // Simple state: just the playback position
        std::vector<uint8_t> state(sizeof(int64_t));
        int64_t pos = playback_position_.load(std::memory_order_acquire);
        std::memcpy(state.data(), &pos, sizeof(int64_t));
        return state;
    }

    void AudioFileSourceNode::loadState(const std::vector<uint8_t>& state) {
        if (state.size() >= sizeof(int64_t)) {
            int64_t pos;
            std::memcpy(&pos, state.data(), sizeof(int64_t));
            playback_position_.store(pos, std::memory_order_release);
        }
    }

} // namespace uapmd
