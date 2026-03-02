#if defined(__EMSCRIPTEN__)

#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "uapmd-data/uapmd-data.hpp"

#include <miniaudio.h>

namespace uapmd {

namespace {

class MiniaudioFileReaderAdapter : public AudioFileReader {
public:
    explicit MiniaudioFileReaderAdapter(const std::string& filepath) {
        ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
        ma_result initResult = MA_ERROR;
#if defined(__EMSCRIPTEN__)
        loadFileIntoMemory(filepath);
        initResult = ma_decoder_init_memory(fileData_.data(),
                                            fileData_.size(),
                                            &config,
                                            &decoder_);
#else
        initResult = ma_decoder_init_file(filepath.c_str(), &config, &decoder_);
#endif
        if (initResult != MA_SUCCESS) {
#if defined(__EMSCRIPTEN__)
            std::cout << "[wasm][miniaudio] decoder init failed for '" << filepath
                      << "' size=" << fileData_.size()
                      << " code=" << initResult
                      << " (" << ma_result_description(initResult) << ")\n";
#else
            std::cout << "[miniaudio] decoder init failed for '" << filepath
                      << "' code=" << initResult
                      << " (" << ma_result_description(initResult) << ")\n";
#endif
            throw std::runtime_error("Failed to initialize decoder");
        }

        props_.numChannels = decoder_.outputChannels;
        props_.sampleRate = decoder_.outputSampleRate;

        ma_uint64 lengthFrames = 0;
        if (ma_decoder_get_length_in_pcm_frames(&decoder_, &lengthFrames) == MA_SUCCESS) {
            props_.numFrames = static_cast<uint64_t>(lengthFrames);
        } else {
            props_.numFrames = determineLengthFallback();
        }

        ma_decoder_seek_to_pcm_frame(&decoder_, 0);
        ensureScratchCapacity();
    }

    ~MiniaudioFileReaderAdapter() override {
        ma_decoder_uninit(&decoder_);
    }

    Properties getProperties() const override {
        return props_;
    }

    void readFrames(uint64_t startFrame,
                    uint64_t framesToRead,
                    float* const* dest,
                    uint32_t numChannels) override {
        if (!dest || numChannels == 0 || framesToRead == 0) {
            return;
        }

        const uint32_t copyChannels = std::min(numChannels, props_.numChannels);
        for (uint32_t ch = 0; ch < numChannels; ++ch) {
            if (dest[ch]) {
                std::fill(dest[ch], dest[ch] + framesToRead, 0.0f);
            }
        }

        if (props_.numChannels == 0 || props_.numFrames == 0 || copyChannels == 0) {
            return;
        }

        if (startFrame >= props_.numFrames) {
            return;
        }

        uint64_t framesRemaining = std::min(framesToRead, props_.numFrames - startFrame);
        if (framesRemaining == 0) {
            return;
        }

        if (ma_decoder_seek_to_pcm_frame(&decoder_, startFrame) != MA_SUCCESS) {
            return;
        }

        ensureScratchCapacity();
        uint64_t destOffset = 0;
        constexpr uint64_t chunkSize = 1024;

        while (framesRemaining > 0) {
            uint64_t request = std::min<uint64_t>(chunkSize, framesRemaining);
            ma_uint64 framesRead = 0;
            auto status = ma_decoder_read_pcm_frames(&decoder_, scratch_.data(), request, &framesRead);
            if ((status != MA_SUCCESS && status != MA_AT_END) || framesRead == 0) {
                break;
            }

            for (uint32_t ch = 0; ch < copyChannels; ++ch) {
                float* out = dest[ch] ? dest[ch] + destOffset : nullptr;
                if (!out) {
                    continue;
                }
                for (ma_uint64 i = 0; i < framesRead; ++i) {
                    out[i] = scratch_[i * props_.numChannels + ch];
                }
            }

            framesRemaining -= framesRead;
            destOffset += framesRead;

            if (framesRead < request || status == MA_AT_END) {
                break;
            }
        }
    }

private:
    uint64_t determineLengthFallback() {
        constexpr uint64_t chunkSize = 2048;
        ensureScratchCapacity(chunkSize);
        uint64_t totalFrames = 0;
        ma_decoder_seek_to_pcm_frame(&decoder_, 0);
        while (true) {
            ma_uint64 framesRead = 0;
            auto status = ma_decoder_read_pcm_frames(&decoder_, scratch_.data(), chunkSize, &framesRead);
            if ((status != MA_SUCCESS && status != MA_AT_END) || framesRead == 0) {
                break;
            }
            totalFrames += framesRead;
            if (status == MA_AT_END) {
                break;
            }
        }
        ma_decoder_seek_to_pcm_frame(&decoder_, 0);
        return totalFrames;
    }

    void ensureScratchCapacity(uint64_t chunkFrames = 1024) {
        const uint32_t channels = std::max<uint32_t>(props_.numChannels, 1);
        const uint64_t required = chunkFrames * channels;
        if (scratch_.size() < required) {
            scratch_.assign(required, 0.0f);
        }
    }

#if defined(__EMSCRIPTEN__)
    void loadFileIntoMemory(const std::string& filepath) {
        std::ifstream file(filepath, std::ios::binary | std::ios::ate);
        if (!file) {
            std::cout << "[wasm][miniaudio] failed to open '" << filepath << "'\n";
            throw std::runtime_error("Failed to open audio file");
        }
        const std::streamsize size = file.tellg();
        if (size <= 0) {
            std::cout << "[wasm][miniaudio] zero-length file '" << filepath << "'\n";
            throw std::runtime_error("Audio file is empty");
        }
        fileData_.resize(static_cast<size_t>(size));
        file.seekg(0, std::ios::beg);
        if (!file.read(reinterpret_cast<char*>(fileData_.data()), size)) {
            std::cout << "[wasm][miniaudio] failed to read '" << filepath << "'\n";
            throw std::runtime_error("Failed to read audio file");
        }
    }
#endif

    ma_decoder decoder_{};
    Properties props_{};
    std::vector<float> scratch_;
#if defined(__EMSCRIPTEN__)
    std::vector<uint8_t> fileData_;
#endif
};

} // namespace

std::unique_ptr<AudioFileReader> createAudioFileReaderFromPath(const std::string& filepath) {
    try {
        return std::make_unique<MiniaudioFileReaderAdapter>(filepath);
    } catch (const std::exception&) {
        return nullptr;
    }
}

} // namespace uapmd

#endif
