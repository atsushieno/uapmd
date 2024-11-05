#pragma once
#include <cstdint>

#include "uapmd/priv/AudioPluginNode.hpp"
#include "uapmd/priv/AudioPluginHostPAL.hpp"
#include "remidy-tooling/PluginScanning.hpp"
#include "remidy-tooling/PluginInstancing.hpp"

namespace uapmd {

    class RemidyAudioPluginHostPAL : public AudioPluginHostPAL {
        remidy_tooling::PluginScanning scanning{};
    public:
        RemidyAudioPluginHostPAL() = default;
        void createPluginInstance(std::string &format, std::string &pluginId, std::function<void(AudioPluginNode* node, std::string error)>&& callback) override;
        uapmd_status_t processAudio(std::vector<remidy::AudioProcessContext*> contexts) override;
    };

}
