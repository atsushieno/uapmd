#pragma once
#include "../Common/CommonTypes.hpp"
#include "AudioPluginGraph.hpp"
#include "remidy/remidy.hpp"

namespace uapmd {

    class AudioPluginTrack {
        class Impl;
        Impl* impl;

    public:
        AudioPluginTrack();
        ~AudioPluginTrack();

        AudioPluginGraph& getGraph();

        bool isBypass();
        bool isFrozen();
        void setBypass(bool value);
        void setFrozen(bool value);

        int32_t processAudio(AudioProcessContext* process);
    };

}
