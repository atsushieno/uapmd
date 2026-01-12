#pragma once

#include <memory>
#include <string>
#include <vector>
#include "uapmd/priv/core/uapmd-core.hpp"

namespace uapmd {
    class AudioPluginNode {
        std::unique_ptr<AudioPluginInstanceAPI> node_;
        bool bypassed_{false};
        int32_t instance_id_;
        std::unique_ptr<UapmdUmpInputMapper> ump_input_mapper;
        std::unique_ptr<UapmdUmpOutputMapper> ump_output_mapper;

    public:
        AudioPluginNode(std::unique_ptr<UapmdUmpInputMapper> umpInputMapper, std::unique_ptr<UapmdUmpOutputMapper> umpOutputMapper, std::unique_ptr<AudioPluginInstanceAPI> nodePAL, int32_t instanceId);
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

        void setUmpOutputMapper(std::unique_ptr<UapmdUmpOutputMapper> mapper);
    };

}
