#pragma once

#include <cstdint>

namespace uapmd {

    // Minimal audio file reader interface to avoid exposing third-party headers
    class AudioFileReader {
    public:
        struct Properties {
            uint64_t numFrames{};
            uint32_t numChannels{};
            uint32_t sampleRate{};
        };

        virtual ~AudioFileReader() = default;

        // Query file properties (number of frames/channels, and sample rate).
        virtual Properties getProperties() const = 0;

        // Read 'framesToRead' frames starting at 'startFrame' into planar buffers.
        // 'dest' is an array of channel pointers with at least 'numChannels' entries.
        // Implementations should fill exactly 'framesToRead' samples per channel.
        virtual void readFrames(uint64_t startFrame,
                                uint64_t framesToRead,
                                float* const* dest,
                                uint32_t numChannels) = 0;
    };

}

