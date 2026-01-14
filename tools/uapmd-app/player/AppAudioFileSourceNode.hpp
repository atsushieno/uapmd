#pragma once

#include <memory>
#include <vector>
#include <atomic>
#include <mutex>
#include "AppSourceNode.hpp"
#include "uapmd-engine/uapmd-engine.hpp"

namespace uapmd_app {

    // App-level audio file source node
    // Plays back audio files as clips on the timeline
    class AppAudioFileSourceNode : public AppSourceNode {
    public:
        AppAudioFileSourceNode(
            int32_t instanceId,
            std::unique_ptr<uapmd::AudioFileReader> reader
        );

        virtual ~AppAudioFileSourceNode() = default;

        // AppAudioNode interface
        int32_t instanceId() const override { return instance_id_; }
        AppNodeType nodeType() const override { return AppNodeType::AudioFileSource; }
        bool bypassed() const override { return bypassed_; }
        void bypassed(bool value) override { bypassed_ = value; }
        std::vector<uint8_t> saveState() override;
        void loadState(const std::vector<uint8_t>& state) override;

        // AppSourceNode interface
        void seek(int64_t samplePosition) override;
        int64_t currentPosition() const override;
        int64_t totalLength() const override;
        bool isPlaying() const override { return is_playing_.load(std::memory_order_acquire); }
        void setPlaying(bool playing) override { is_playing_.store(playing, std::memory_order_release); }
        void processAudio(float** buffers, uint32_t numChannels, int32_t frameCount) override;
        uint32_t channelCount() const override { return channel_count_; }

        // Audio file specific
        double sampleRate() const { return sample_rate_; }
        int64_t numFrames() const { return num_frames_; }

    private:
        int32_t instance_id_;
        bool bypassed_{false};
        std::atomic<bool> is_playing_{false};
        std::atomic<int64_t> playback_position_{0};

        // Audio file data (loaded into memory)
        std::unique_ptr<uapmd::AudioFileReader> reader_;
        std::vector<std::vector<float>> audio_buffer_;  // Per-channel planar buffers
        uint32_t channel_count_{0};
        int64_t num_frames_{0};
        double sample_rate_{0.0};

        std::mutex buffer_mutex_;  // Protects audio_buffer_ during loading
    };

} // namespace uapmd_app
