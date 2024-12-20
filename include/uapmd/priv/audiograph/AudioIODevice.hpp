#pragma once

#include <string>

namespace uapmd {
    class AudioIODevice {
    protected:
        AudioIODevice() = default;
        virtual ~AudioIODevice() = default;

    public:
        static AudioIODevice* instance(const std::string& deviceName = "", const std::string& driverName = "");

        virtual void addAudioCallback(std::function<uapmd_status_t(AudioProcessContext& data)>&& callback) = 0;
        virtual void clearAudioCallbacks() = 0;
        virtual double sampleRate() = 0;
        virtual uint32_t channels() = 0;
        virtual uapmd_status_t start() = 0;
        virtual uapmd_status_t stop() = 0;
        virtual bool isPlaying() = 0;
    };
}