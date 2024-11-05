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

        std::vector<AudioPluginTrack*>& tracks();

        void addSimpleTrack(std::string& format, std::string& pluginId, std::function<void(std::string error)>&& callback);

        uapmd_status_t processAudio(std::vector<remidy::AudioProcessContext*> contexts);
    };

}
