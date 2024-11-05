#pragma once

#include "AudioIODriver.hpp"

namespace uapmd {
    class AudioIOService {
        class Impl;
        Impl* impl;
    public:
        explicit AudioIOService(AudioIODriver* driver = nullptr);
        ~AudioIOService();

        void addAudioCallback(std::function<uapmd_status_t(AudioProcessContext& data)>&& callback);
        uapmd_status_t start();
        uapmd_status_t stop();
    };
}

