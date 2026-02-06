#pragma once

#include "AudioSourceNode.hpp"
#include <vector>
#include <atomic>

namespace uapmd {

    // Device input source node
    // Captures device input (microphone/line-in) and makes it available as a source
    class DeviceInputSourceNode : public AudioSourceNode {
    public:
        DeviceInputSourceNode(
            int32_t instanceId,
            uint32_t channelCount,
            const std::vector<uint32_t>& inputChannelIndices = {}
        );

        virtual ~DeviceInputSourceNode() = default;

        // SourceNode interface
        int32_t instanceId() const override { return instance_id_; }
        SourceNodeType nodeType() const override { return SourceNodeType::DeviceInput; }
        bool bypassed() const override { return bypassed_; }
        void bypassed(bool value) override { bypassed_ = value; }
        std::vector<uint8_t> saveState() override;
        void loadState(const std::vector<uint8_t>& state) override;

        // AudioSourceNode interface
        void seek(int64_t samplePosition) override {} // No-op for live input
        int64_t currentPosition() const override { return 0; }
        int64_t totalLength() const override { return INT64_MAX; }
        bool isPlaying() const override { return is_active_.load(std::memory_order_acquire); }
        void setPlaying(bool playing) override { is_active_.store(playing, std::memory_order_release); }
        void processAudio(float** buffers, uint32_t numChannels, int32_t frameCount) override;
        uint32_t channelCount() const override { return channel_count_; }

        // Device input specific
        void setInputChannels(const std::vector<uint32_t>& channelIndices);
        const std::vector<uint32_t>& getInputChannels() const { return input_channel_indices_; }

        // Set the device input buffers to read from
        void setDeviceInputBuffers(float** deviceBuffers, uint32_t deviceChannelCount);

    private:
        int32_t instance_id_;
        bool bypassed_{false};
        std::atomic<bool> is_active_{false};
        uint32_t channel_count_;
        std::vector<uint32_t> input_channel_indices_;  // Map to device channels

        // Temporary storage for device input buffer pointers (set per process call)
        float** device_input_buffers_{nullptr};
        uint32_t device_channel_count_{0};
    };

} // namespace uapmd
