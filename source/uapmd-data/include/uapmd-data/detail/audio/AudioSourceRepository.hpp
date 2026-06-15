#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>

#include "AudioFileFactory.hpp"

namespace uapmd {

    struct AudioSourceInfo {
        std::string audioSourceId;
        std::string filepath;
        uint32_t channelCount{0};
        double sampleRate{0.0};
        int64_t frameCount{0};
    };

    class AudioSourceRepository {
    public:
        virtual ~AudioSourceRepository() = default;

        virtual std::optional<AudioSourceInfo> getAudioSourceInfo(
            const std::string& audioSourceId,
            const std::string& filepath) const = 0;

        virtual bool readAudioSourceSamples(
            const std::string& audioSourceId,
            const std::string& filepath,
            int64_t startFrame,
            int64_t frameCount,
            float** destination,
            uint32_t destinationChannels) const = 0;
    };

    class FileAudioSourceRepository : public AudioSourceRepository {
    public:
        std::optional<AudioSourceInfo> getAudioSourceInfo(
            const std::string& audioSourceId,
            const std::string& filepath) const override {
            if (filepath.empty())
                return std::nullopt;
            auto reader = createAudioFileReaderFromPath(filepath);
            if (!reader)
                return std::nullopt;

            const auto props = reader->getProperties();
            return AudioSourceInfo{
                .audioSourceId = audioSourceId,
                .filepath = filepath,
                .channelCount = props.numChannels,
                .sampleRate = static_cast<double>(props.sampleRate),
                .frameCount = static_cast<int64_t>(props.numFrames)
            };
        }

        bool readAudioSourceSamples(
            const std::string& audioSourceId,
            const std::string& filepath,
            int64_t startFrame,
            int64_t frameCount,
            float** destination,
            uint32_t destinationChannels) const override {
            (void) audioSourceId;
            if (filepath.empty() || !destination || startFrame < 0 || frameCount < 0)
                return false;

            for (uint32_t ch = 0; ch < destinationChannels; ++ch)
                if (destination[ch])
                    std::memset(destination[ch], 0, static_cast<size_t>(frameCount) * sizeof(float));

            if (frameCount == 0)
                return true;

            auto reader = createAudioFileReaderFromPath(filepath);
            if (!reader)
                return false;

            const auto props = reader->getProperties();
            if (startFrame >= static_cast<int64_t>(props.numFrames))
                return true;

            const auto framesToRead = std::min<int64_t>(
                frameCount,
                static_cast<int64_t>(props.numFrames) - startFrame);
            if (framesToRead <= 0)
                return true;

            reader->readFrames(
                static_cast<uint64_t>(startFrame),
                static_cast<uint64_t>(framesToRead),
                destination,
                std::min(destinationChannels, props.numChannels));
            return true;
        }
    };

} // namespace uapmd
