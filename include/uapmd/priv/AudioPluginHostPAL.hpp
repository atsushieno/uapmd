#pragma once

#include "uapmd/uapmd.hpp"

namespace uapmd {

    class AudioPluginHostPAL {
    public:
        static AudioPluginHostPAL* instance();
        virtual ~AudioPluginHostPAL() = default;
        virtual void createPluginInstance(std::string &format, std::string &pluginId, std::function<void(AudioPluginNode* node, std::string error)>&& callback) = 0;
        virtual uapmd_status_t processAudio(std::vector<remidy::AudioProcessContext*> contexts) = 0;
    };

}