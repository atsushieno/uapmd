#pragma once
#include <vector>

#include "AudioPluginNode.hpp"
#include "uapmd/priv/CommonTypes.hpp"

namespace uapmd {

    class AudioPluginGraph {
        class Impl;
        Impl* impl{};
    public:
        AudioPluginGraph();
        ~AudioPluginGraph();

        uapmd_status_t appendNodeSimple(std::unique_ptr<AudioPluginNode> newNode);

        std::vector<AudioPluginNode*> plugins();

        int32_t processAudio(AudioProcessContext& process);
    };

}
