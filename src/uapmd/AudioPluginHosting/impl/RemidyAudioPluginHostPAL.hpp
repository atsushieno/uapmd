#pragma once
#include <cstdint>

#include "uapmd/priv/plugingraph/AudioPluginNode.hpp"
#include "uapmd/priv/plugingraph/AudioPluginHostPAL.hpp"
#include "remidy-tooling/PluginScanTool.hpp"
#include "remidy-tooling/PluginInstancing.hpp"

namespace uapmd {

    class RemidyAudioPluginHostPAL : public AudioPluginHostPAL {
        remidy_tooling::PluginScanTool scanning{};
    public:
        RemidyAudioPluginHostPAL();
        ~RemidyAudioPluginHostPAL() override = default;
        remidy::PluginCatalog& catalog() override { return scanning.catalog; }
        void performPluginScanning(bool rescan) override;
        void createPluginInstance(uint32_t sampleRate, std::string &format, std::string &pluginId, std::function<void(std::unique_ptr<AudioPluginNode> node, std::string error)>&& callback) override;
        uapmd_status_t processAudio(std::vector<remidy::AudioProcessContext*> contexts) override;
    };

}
