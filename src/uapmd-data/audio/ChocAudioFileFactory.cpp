#include <algorithm>
#include <cctype>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "uapmd-data/uapmd-data.hpp"

#include <choc/audio/choc_AudioFileFormat_WAV.h>
#include <choc/audio/choc_AudioFileFormat_FLAC.h>
#include <choc/audio/choc_AudioFileFormat_Ogg.h>
#include <choc/audio/choc_SampleBuffers.h>

namespace uapmd {

    namespace {
#if defined(__EMSCRIPTEN__)
        std::vector<char> loadFileIntoMemoryBuffer(const std::string& filepath) {
            std::ifstream file(filepath, std::ios::binary | std::ios::ate);
            if (!file)
                return {};

            const auto size = file.tellg();
            if (size <= 0)
                return {};

            std::vector<char> buffer(static_cast<size_t>(size));
            file.seekg(0, std::ios::beg);
            if (!file.read(buffer.data(), static_cast<std::streamsize>(buffer.size())))
                return {};

            return buffer;
        }

        std::shared_ptr<std::istream> makeStreamFromBuffer(const std::vector<char>& buffer) {
            auto stream = std::make_shared<std::stringstream>(std::ios::in | std::ios::binary);
            stream->write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            stream->seekg(0, std::ios::beg);
            stream->clear();
            return stream;
        }
#endif

        class ChocAudioFileReaderAdapter : public AudioFileReader {
        public:
            explicit ChocAudioFileReaderAdapter(std::unique_ptr<choc::audio::AudioFileReader>&& reader)
                : impl_(std::move(reader)) {}

            Properties getProperties() const override {
                auto p = impl_->getProperties();
                return Properties{ p.numFrames, p.numChannels, static_cast<uint32_t>(p.sampleRate) };
            }

            void readFrames(uint64_t startFrame,
                            uint64_t framesToRead,
                            float* const* dest,
                            uint32_t numChannels) override {
                choc::buffer::ChannelArrayBuffer<float> temp(numChannels, framesToRead);
                impl_->readFrames(startFrame, temp.getView());
                for (uint32_t ch = 0; ch < numChannels; ++ch) {
                    float* out = dest[ch];
                    for (uint64_t i = 0; i < framesToRead; ++i)
                        out[i] = temp.getSample(ch, i);
                }
            }

        private:
            std::unique_ptr<choc::audio::AudioFileReader> impl_;
        };
    }

    std::unique_ptr<AudioFileReader> createAudioFileReaderFromPath(const std::string& filepath) {
#if defined(__EMSCRIPTEN__)
        const auto wasmBuffer = loadFileIntoMemoryBuffer(filepath);
        if (wasmBuffer.empty())
            return nullptr;

        auto createReaderForTarget = [&](auto format) -> std::unique_ptr<choc::audio::AudioFileReader> {
            return format.createReader(makeStreamFromBuffer(wasmBuffer));
        };
#else
        auto createReaderForTarget = [&](auto format) -> std::unique_ptr<choc::audio::AudioFileReader> {
            return format.createReader(filepath);
        };
#endif

        std::string ext;
        auto dot = filepath.find_last_of('.');
        if (dot != std::string::npos)
            ext = filepath.substr(dot + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){ return std::tolower(c); });

        std::unique_ptr<choc::audio::AudioFileReader> reader;

        if (ext == "wav") {
            reader = createReaderForTarget(choc::audio::WAVAudioFileFormat<true>());
        } else if (ext == "flac") {
            reader = createReaderForTarget(choc::audio::FLACAudioFileFormat<true>());
        } else if (ext == "ogg") {
            reader = createReaderForTarget(choc::audio::OggAudioFileFormat<true>());
        } else {
            // Try all formats as fallback
            reader = createReaderForTarget(choc::audio::WAVAudioFileFormat<true>());
            if (!reader)
                reader = createReaderForTarget(choc::audio::FLACAudioFileFormat<true>());
            if (!reader)
                reader = createReaderForTarget(choc::audio::OggAudioFileFormat<true>());
        }

        if (!reader)
            return nullptr;
        return std::make_unique<ChocAudioFileReaderAdapter>(std::move(reader));
    }

}
