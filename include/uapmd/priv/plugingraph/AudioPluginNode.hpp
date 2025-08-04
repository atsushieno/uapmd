#pragma once

#include <string>
#include "uapmd/priv/CommonTypes.hpp"
#include "AudioPluginHostPAL.hpp"

namespace uapmd {

    struct ParameterMetadata {
        uint32_t index;
        std::string stableId;
        std::string name;
        std::string path;
        double initialValue;
        bool hidden;
    };

    class AudioPluginNode {
        class Impl;
        Impl* impl;
    public:
        AudioPluginNode(std::unique_ptr<AudioPluginHostPAL::AudioPluginNodePAL> nodePAL, int32_t instanceId);
        virtual ~AudioPluginNode();

        AudioPluginHostPAL::AudioPluginNodePAL* pal();

        // instanceId can be used to indicate a plugin instance *across* processes i.e.
        // where pointer to the instance cannot be shared.
        int32_t instanceId();

        bool bypassed();
        void bypassed(bool value);

        uapmd_status_t processAudio(AudioProcessContext& process);

        std::vector<ParameterMetadata> parameterMetadataList(int32_t instanceId);

        void loadState(std::vector<uint8_t>& state);
        std::vector<uint8_t> saveState();
    };

}
