#pragma once
#include <vector>

#include "AudioPluginNode.hpp"
#include "CommonTypes.hpp"
#include "remidy/remidy.hpp"

namespace uapmd {

    class AudioPluginGraph {
        class Impl;
        Impl* impl{};
    public:
        AudioPluginGraph();
        ~AudioPluginGraph();

        uapmd_status_t appendNodeSimple(AudioPluginNode* newNode);

        int32_t processAudio(AudioProcessContext& process);
    };

}
