#pragma once
#include "uapmd/priv/CommonTypes.hpp"
#include "AudioPluginGraph.hpp"
#include "remidy/remidy.hpp"

namespace uapmd {

    class AudioPluginTrack {
        class Impl;
        Impl* impl;

    public:
        AudioPluginTrack();
        ~AudioPluginTrack();

        AudioPluginGraph& graph();

        bool bypassed();
        bool frozen();
        void bypassed(bool value);
        void frozen(bool value);

        int32_t processAudio(AudioProcessContext& process);
    };

}
