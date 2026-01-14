#pragma once
#include <memory>
#include <vector>

#include "AudioPluginNode.hpp"
#include "uapmd/uapmd.hpp"

namespace uapmd {

    class AudioPluginGraph {
    protected:
        AudioPluginGraph() = default;

    public:
        virtual ~AudioPluginGraph() = default;
        static std::unique_ptr<AudioPluginGraph> create();

        virtual uapmd_status_t appendNodeSimple(std::unique_ptr<AudioPluginNode> newNode) = 0;
        virtual bool removePluginInstance(int32_t instanceId) = 0;

        virtual std::vector<AudioPluginNode*> plugins() = 0;

        virtual int32_t processAudio(AudioProcessContext& process) = 0;
    };

}
