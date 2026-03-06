#pragma once

#if defined(__ANDROID__)

#include "uapmd/uapmd.hpp"
#include "uapmd-engine/uapmd-engine.hpp"
#include <oboe/Oboe.h>
#include <functional>
#include <memory>
#include <vector>

namespace uapmd {

    class OboeAudioIODevice : public AudioIODevice, public oboe::AudioStreamCallback {
        struct StreamCloser {
            void operator()(oboe::AudioStream* stream) const {
                if (stream)
                    stream->close();
            }
        };

        Logger* logger_{nullptr};
        MasterContext master_context{};
        AudioProcessContext data;
        std::vector<std::function<uapmd_status_t(AudioProcessContext&)>> callbacks{};
        std::vector<float*> dataOutPtrs{};
        std::unique_ptr<oboe::AudioStream, StreamCloser> stream_{};

        uint32_t requested_sample_rate_{48000};
        uint32_t requested_output_channels_{2};
        uint32_t requested_buffer_size_{0};

        uint32_t input_channels_{0};
        uint32_t output_channels_{2};
        uint32_t buffer_capacity_frames_{0};
        double sample_rate_{48000.0};
        bool playing_{false};

        bool openStream();
        void closeStream();

    public:
        explicit OboeAudioIODevice(Logger* logger);
        ~OboeAudioIODevice() override;

        void addAudioCallback(std::function<uapmd_status_t(AudioProcessContext&)> &&callback) override;
        void clearAudioCallbacks() override;

        double sampleRate() override { return sample_rate_; }
        uint32_t channels() override { return output_channels_; }
        uint32_t inputChannels() override { return input_channels_; }
        uint32_t outputChannels() override { return output_channels_; }
        std::vector<uint32_t> getNativeSampleRates() override;

        uapmd_status_t start() override;
        uapmd_status_t stop() override;
        bool isPlaying() override { return playing_; }

        bool reconfigure(uint32_t sampleRateHint, uint32_t channelCountHint = 2, uint32_t bufferSizeHint = 0);

        oboe::DataCallbackResult onAudioReady(oboe::AudioStream* audioStream,
                                              void* audioData,
                                              int32_t numFrames) override;
        bool onError(oboe::AudioStream* audioStream, oboe::Result error) override;
    };

    class OboeAudioIODeviceManager : public AudioIODeviceManager {
        Logger* logger_{nullptr};
        std::unique_ptr<OboeAudioIODevice> audio_{};

    protected:
        std::vector<AudioIODeviceInfo> onDevices() override;
        AudioIODevice* onOpen(int inputDeviceIndex,
                              int outputDeviceIndex,
                              uint32_t sampleRate,
                              uint32_t bufferSize) override;

    public:
        OboeAudioIODeviceManager() : AudioIODeviceManager("oboe") {}
        ~OboeAudioIODeviceManager() override = default;

        void initialize(Configuration& config) override;
        std::vector<uint32_t> getDeviceSampleRates(const std::string& deviceName,
                                                   AudioIODirections direction) override;
    };
}

#endif // __ANDROID__
