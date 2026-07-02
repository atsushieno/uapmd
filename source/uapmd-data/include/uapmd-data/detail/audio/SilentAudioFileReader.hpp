#pragma once

#include <algorithm>
#include "AudioFileReader.hpp"

namespace uapmd {

    // AudioFileReader backed by silence rather than a file, for creating blank audio clips.
    class SilentAudioFileReader : public AudioFileReader {
    public:
        SilentAudioFileReader(uint64_t numFrames, uint32_t numChannels, uint32_t sampleRate)
            : properties_{numFrames, numChannels, sampleRate} {}

        Properties getProperties() const override { return properties_; }

        void readFrames(uint64_t /*startFrame*/,
                        uint64_t framesToRead,
                        float* const* dest,
                        uint32_t numChannels) override {
            for (uint32_t ch = 0; ch < numChannels; ch++)
                std::fill_n(dest[ch], framesToRead, 0.0f);
        }

    private:
        Properties properties_;
    };

} // namespace uapmd
