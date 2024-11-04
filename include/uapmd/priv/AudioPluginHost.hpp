#pragma once
#include <memory>

#include "AudioPluginTrack.hpp"
#include "remidy/remidy.hpp"

namespace uapmd {

    class AudioPluginHost {
        class Impl;
        Impl *impl;

    public:
        AudioPluginHost();
        ~AudioPluginHost();

        AudioPluginTrack* getTrack(int32_t index);
        uapmd_status_t processAudio(AudioProcessContext* process);
    };

}
