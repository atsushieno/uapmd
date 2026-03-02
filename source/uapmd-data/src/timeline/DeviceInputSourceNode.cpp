#include <algorithm>
#include <cstring>
#include "uapmd-engine/uapmd-engine.hpp"

namespace uapmd {

    DeviceInputSourceNode::DeviceInputSourceNode(
        int32_t instanceId,
        uint32_t channelCount,
        const std::vector<uint32_t>& inputChannelIndices
    ) : instance_id_(instanceId),
        channel_count_(channelCount),
        input_channel_indices_(inputChannelIndices) {

        // If no explicit channel mapping provided, use 1:1 mapping
        if (input_channel_indices_.empty()) {
            for (uint32_t i = 0; i < channel_count_; ++i) {
                input_channel_indices_.push_back(i);
            }
        }
    }

    void DeviceInputSourceNode::setInputChannels(const std::vector<uint32_t>& channelIndices) {
        input_channel_indices_ = channelIndices;
    }

    void DeviceInputSourceNode::setDeviceInputBuffers(float** deviceBuffers, uint32_t deviceChannelCount) {
        device_input_buffers_ = deviceBuffers;
        device_channel_count_ = deviceChannelCount;
    }

    void DeviceInputSourceNode::processAudio(float** buffers, uint32_t numChannels, int32_t frameCount) {
        // Clear output buffers first
        for (uint32_t ch = 0; ch < numChannels; ++ch) {
            if (buffers[ch])
                std::memset(buffers[ch], 0, frameCount * sizeof(float));
        }

        if (bypassed_ || !is_active_.load(std::memory_order_acquire))
            return;

        if (!device_input_buffers_)
            return;

        // Copy device input to output buffers based on channel mapping
        for (uint32_t outCh = 0; outCh < std::min(numChannels, channel_count_); ++outCh) {
            if (!buffers[outCh])
                continue;

            // Get the mapped device input channel
            if (outCh >= input_channel_indices_.size())
                continue;

            uint32_t deviceCh = input_channel_indices_[outCh];
            if (deviceCh >= device_channel_count_)
                continue;

            if (!device_input_buffers_[deviceCh])
                continue;

            // Copy device input to output
            std::memcpy(buffers[outCh], device_input_buffers_[deviceCh], frameCount * sizeof(float));
        }
    }

    std::vector<uint8_t> DeviceInputSourceNode::saveState() {
        // Save channel mapping
        std::vector<uint8_t> state;
        size_t numIndices = input_channel_indices_.size();
        state.resize(sizeof(size_t) + numIndices * sizeof(uint32_t));

        uint8_t* ptr = state.data();
        std::memcpy(ptr, &numIndices, sizeof(size_t));
        ptr += sizeof(size_t);

        for (size_t i = 0; i < numIndices; ++i) {
            std::memcpy(ptr, &input_channel_indices_[i], sizeof(uint32_t));
            ptr += sizeof(uint32_t);
        }

        return state;
    }

    void DeviceInputSourceNode::loadState(const std::vector<uint8_t>& state) {
        if (state.size() < sizeof(size_t))
            return;

        const uint8_t* ptr = state.data();
        size_t numIndices;
        std::memcpy(&numIndices, ptr, sizeof(size_t));
        ptr += sizeof(size_t);

        if (state.size() < sizeof(size_t) + numIndices * sizeof(uint32_t))
            return;

        input_channel_indices_.clear();
        input_channel_indices_.reserve(numIndices);

        for (size_t i = 0; i < numIndices; ++i) {
            uint32_t index;
            std::memcpy(&index, ptr, sizeof(uint32_t));
            ptr += sizeof(uint32_t);
            input_channel_indices_.push_back(index);
        }
    }

} // namespace uapmd
