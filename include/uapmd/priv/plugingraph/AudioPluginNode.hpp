#pragma once

#include <string>
#include "uapmd/priv/CommonTypes.hpp"
#include "AudioPluginHostPAL.hpp"

namespace uapmd {

    class AudioPluginNode {
        class Impl;
        Impl* impl;
    public:
        AudioPluginNode(std::unique_ptr<AudioPluginHostPAL::AudioPluginNodePAL> nodePAL);
        virtual ~AudioPluginNode();

        AudioPluginHostPAL::AudioPluginNodePAL* pal();

        bool bypassed();
        void bypassed(bool value);

        uapmd_status_t processAudio(AudioProcessContext& process);
    };

}
