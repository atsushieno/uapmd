#include <algorithm>
#include <cctype>
#include <memory>
#include <string>
#include <vector>

#include "uapmd/priv/audio/AudioFileFactory.hpp"

#include <choc/audio/choc_AudioFileFormat_WAV.h>
#include <choc/audio/choc_AudioFileFormat_FLAC.h>
#include <choc/audio/choc_AudioFileFormat_Ogg.h>
#include <choc/audio/choc_SampleBuffers.h>

namespace uapmd {

    namespace {
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
        std::string ext;
        auto dot = filepath.find_last_of('.');
        if (dot != std::string::npos)
            ext = filepath.substr(dot + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){ return std::tolower(c); });

        std::unique_ptr<choc::audio::AudioFileReader> reader;

        if (ext == "wav") {
            reader = choc::audio::WAVAudioFileFormat<true>().createReader(filepath);
        } else if (ext == "flac") {
            reader = choc::audio::FLACAudioFileFormat<true>().createReader(filepath);
        } else if (ext == "ogg") {
            reader = choc::audio::OggAudioFileFormat<true>().createReader(filepath);
        } else {
            // Try all formats as fallback
            reader = choc::audio::WAVAudioFileFormat<true>().createReader(filepath);
            if (!reader)
                reader = choc::audio::FLACAudioFileFormat<true>().createReader(filepath);
            if (!reader)
                reader = choc::audio::OggAudioFileFormat<true>().createReader(filepath);
        }

        if (!reader)
            return nullptr;
        return std::make_unique<ChocAudioFileReaderAdapter>(std::move(reader));
    }

}

