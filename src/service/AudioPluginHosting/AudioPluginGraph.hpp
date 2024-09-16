#pragma once
#include <vector>

#include "AudioPluginNode.hpp"
#include "../Common/CommonTypes.hpp"
#include "remidy/remidy.hpp"

namespace uapmd {

    class AudioPluginGraph {
        class Impl;
        Impl* impl{};
    public:
        AudioPluginGraph();
        ~AudioPluginGraph();
        int32_t processAudio(AudioProcessContext* process);
    };

}
