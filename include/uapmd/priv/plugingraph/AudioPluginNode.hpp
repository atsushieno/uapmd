#pragma once

#include <string>
#include "uapmd/priv/CommonTypes.hpp"

namespace uapmd {

    class AudioPluginNode {
        class Impl;
        Impl* impl;
    public:
        AudioPluginNode(std::string& formatName, std::string& pluginId);
        virtual ~AudioPluginNode();

        std::string& formatName() const;
        std::string& pluginId() const;
        bool bypassed();
        void bypassed(bool value);

        uapmd_status_t processAudio(AudioProcessContext& process);
    };

}
