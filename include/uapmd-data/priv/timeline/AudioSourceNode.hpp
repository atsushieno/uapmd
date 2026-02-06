#pragma once

#include "SourceNode.hpp"
#include <cstdint>

namespace uapmd {

    // Abstract base class for all audio source nodes (generate audio, not process)
    class AudioSourceNode : public SourceNode {
    public:
        virtual ~AudioSourceNode() = default;

        // Source-specific interface
        virtual void seek(int64_t samplePosition) = 0;
        virtual int64_t currentPosition() const = 0;
        virtual int64_t totalLength() const = 0;
        virtual bool isPlaying() const = 0;
        virtual void setPlaying(bool playing) = 0;

        // Audio generation
        // Generates audio into the provided buffers
        // buffers: array of float* pointers (one per channel)
        // numChannels: number of channels
        // frameCount: number of frames to generate
        virtual void processAudio(float** buffers, uint32_t numChannels, int32_t frameCount) = 0;

        // Get channel count for this source
        virtual uint32_t channelCount() const = 0;
    };

} // namespace uapmd
