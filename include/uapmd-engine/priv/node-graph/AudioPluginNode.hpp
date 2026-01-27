#pragma once

#include "uapmd/uapmd.hpp"

namespace uapmd {
    class AudioPluginNode {
        std::unique_ptr<AudioPluginInstanceAPI> node_;
        bool bypassed_{true}; // initial
        int32_t instance_id_;

    public:
        AudioPluginNode(std::unique_ptr<AudioPluginInstanceAPI> nodePAL, int32_t instanceId);
        virtual ~AudioPluginNode();

        AudioPluginInstanceAPI* pal();

        // instanceId can be used to indicate a plugin instance *across* processes i.e.
        // where pointer to the instance cannot be shared.
        int32_t instanceId();

        bool bypassed();
        void bypassed(bool value);

        uapmd_status_t processAudio(AudioProcessContext& process);

        void loadState(std::vector<uint8_t>& state);
        std::vector<uint8_t> saveState();
    };

}
