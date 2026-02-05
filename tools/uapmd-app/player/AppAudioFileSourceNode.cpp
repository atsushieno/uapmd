#include <algorithm>
#include <cstring>
#include "AppAudioFileSourceNode.hpp"
#include "uapmd-engine/uapmd-engine.hpp"

namespace uapmd_app {

    AppAudioFileSourceNode::AppAudioFileSourceNode(
        int32_t instanceId,
        std::unique_ptr<uapmd::AudioFileReader> reader
    ) : instance_id_(instanceId), reader_(std::move(reader)) {

        if (!reader_)
            return;

        // Get file properties
        const auto& props = reader_->getProperties();
        channel_count_ = props.numChannels;
        num_frames_ = props.numFrames;
        sample_rate_ = props.sampleRate;

        // Load entire file into memory (matching current SequencerEngine behavior)
        std::lock_guard<std::mutex> lock(buffer_mutex_);
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
    }

    void AppAudioFileSourceNode::seek(int64_t samplePosition) {
        playback_position_.store(samplePosition, std::memory_order_release);
    }

    int64_t AppAudioFileSourceNode::currentPosition() const {
        return playback_position_.load(std::memory_order_acquire);
    }

    int64_t AppAudioFileSourceNode::totalLength() const {
        return num_frames_;
    }

    void AppAudioFileSourceNode::processAudio(float** buffers, uint32_t numChannels, int32_t frameCount) {
        // Clear output buffers first
        for (uint32_t ch = 0; ch < numChannels; ++ch) {
            if (buffers[ch])
                std::memset(buffers[ch], 0, frameCount * sizeof(float));
        }

        if (bypassed_ || !is_playing_.load(std::memory_order_acquire))
            return;

        if (audio_buffer_.empty())
            return;

        int64_t pos = playback_position_.load(std::memory_order_acquire);

        // Check if we're within bounds
        if (pos < 0 || pos >= num_frames_)
            return;

        // Calculate how many frames we can actually copy
        int64_t framesToCopy = std::min(static_cast<int64_t>(frameCount), num_frames_ - pos);
        if (framesToCopy <= 0)
            return;

        // Copy audio data from our buffer to output buffers
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        for (uint32_t ch = 0; ch < std::min(numChannels, channel_count_); ++ch) {
            if (buffers[ch] && ch < audio_buffer_.size()) {
                const float* src = &audio_buffer_[ch][pos];
                std::memcpy(buffers[ch], src, framesToCopy * sizeof(float));
            }
        }

        // Advance playback position
        playback_position_.store(pos + framesToCopy, std::memory_order_release);
    }

    std::vector<uint8_t> AppAudioFileSourceNode::saveState() {
        // Simple state: just the playback position
        std::vector<uint8_t> state(sizeof(int64_t));
        int64_t pos = playback_position_.load(std::memory_order_acquire);
        std::memcpy(state.data(), &pos, sizeof(int64_t));
        return state;
    }

    void AppAudioFileSourceNode::loadState(const std::vector<uint8_t>& state) {
        if (state.size() >= sizeof(int64_t)) {
            int64_t pos;
            std::memcpy(&pos, state.data(), sizeof(int64_t));
            playback_position_.store(pos, std::memory_order_release);
        }
    }

} // namespace uapmd_app
