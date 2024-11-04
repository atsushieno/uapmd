#pragma once

#include <string>

namespace uapmd {
    class AudioIODriver {
    protected:
        AudioIODriver() = default;
        virtual ~AudioIODriver() = default;

    public:
        static AudioIODriver* instance(std::string driverName = "");

        virtual void addAudioCallback(std::function<uapmd_status_t(AudioProcessContext& data)>&& callback) = 0;
        virtual uapmd_status_t start() = 0;
        virtual uapmd_status_t stop() = 0;
    };
}