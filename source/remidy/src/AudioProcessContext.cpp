
#include "remidy/priv/processing-context.hpp"

namespace remidy {
    void AudioProcessContext::rebuildBuses(std::vector<AudioBusBufferList*>& buses, std::vector<AudioBusSpec>& specsStorage, const std::vector<AudioBusSpec>& requestedSpecs) {
        for (auto* bus : buses)
            delete bus;
        buses.clear();
        specsStorage = requestedSpecs;
        for (auto& spec : specsStorage) {
            size_t capacity = spec.bufferCapacityFrames;
            if (capacity == 0)
                capacity = audio_buffer_capacity_frames > 0 ? audio_buffer_capacity_frames : 1;
            if (capacity > audio_buffer_capacity_frames)
                audio_buffer_capacity_frames = capacity;
            auto* bus = new AudioBusBufferList(spec.channels, static_cast<uint32_t>(capacity), spec.role);
            buses.emplace_back(bus);
        }
    }

    void AudioProcessContext::copyInputsToOutputs() {
        auto dataType = master_context.audioDataType();
        size_t busCount = std::min(audio_in.size(), audio_out.size());
        const size_t frames = static_cast<size_t>(std::max(frame_count, 0));
        for (size_t i = 0; i < busCount; ++i) {
            auto* inBus = audio_in[i];
            auto* outBus = audio_out[i];
            if (!inBus || !outBus)
                continue;
            auto channels = std::min(inBus->channelCount(), outBus->channelCount());
            auto maxFrames = std::min(
                {static_cast<size_t>(inBus->bufferCapacityInFrames()),
                 static_cast<size_t>(outBus->bufferCapacityInFrames()),
                 frames});
            for (uint32_t ch = 0; ch < channels; ++ch) {
                if (dataType == AudioContentType::Float64) {
                    auto* src = inBus->getDoubleBufferForChannel(ch);
                    auto* dst = outBus->getDoubleBufferForChannel(ch);
                    if (src && dst && maxFrames > 0)
                        std::memcpy(dst, src, maxFrames * sizeof(double));
                } else {
                    auto* src = inBus->getFloatBufferForChannel(ch);
                    auto* dst = outBus->getFloatBufferForChannel(ch);
                    if (src && dst && maxFrames > 0)
                        std::memcpy(dst, src, maxFrames * sizeof(float));
                }
            }
        }
    }


    void AudioProcessContext::enableReplacingIO() {
        if (replacing_enabled_)
            return;
        size_t busCount = std::min(audio_in.size(), audio_out.size());
        for (size_t i = 0; i < busCount; ++i) {
            if (audio_in[i] && audio_out[i])
                audio_in[i]->aliasFrom(*audio_out[i]);
        }
        replacing_enabled_ = true;
    }

    void AudioProcessContext::disableReplacingIO() {
        if (!replacing_enabled_)
            return;
        for (auto* bus : audio_in)
            if (bus)
                bus->useOwnedData();
        replacing_enabled_ = false;
    }

    void AudioProcessContext::advanceToNextNode() {
        auto dataType = master_context.audioDataType();
        const size_t frames = static_cast<size_t>(std::max(frame_count, 0));
        // Copy audio output to input for the next node
        for (size_t i = 0; i < std::min(audio_in.size(), audio_out.size()); ++i) {
            auto* inBus = audio_in[i];
            auto* outBus = audio_out[i];
            if (inBus && outBus) {
                auto channels = std::min(inBus->channelCount(), outBus->channelCount());
                auto maxFrames = std::min(
                    {static_cast<size_t>(inBus->bufferCapacityInFrames()),
                     static_cast<size_t>(outBus->bufferCapacityInFrames()),
                     frames});
                for (uint32_t ch = 0; ch < channels; ++ch) {
                    if (dataType == AudioContentType::Float64) {
                        auto* dst = inBus->getDoubleBufferForChannel(ch);
                        auto* src = outBus->getDoubleBufferForChannel(ch);
                        if (dst && src && maxFrames > 0)
                            std::memcpy(dst, src, maxFrames * sizeof(double));
                    } else {
                        auto* dst = inBus->getFloatBufferForChannel(ch);
                        auto* src = outBus->getFloatBufferForChannel(ch);
                        if (dst && src && maxFrames > 0)
                            std::memcpy(dst, src, maxFrames * sizeof(float));
                    }
                }
            }
        }

        // Clear output buffers for the next plugin
        for (auto* bus : audio_out)
            if (bus)
                bus->clear();

        event_out.position(0);
    }
}