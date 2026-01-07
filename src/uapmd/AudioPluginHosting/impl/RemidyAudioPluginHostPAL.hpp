#pragma once
#include <cstdint>
#include <atomic>

#include "uapmd/priv/plugingraph/AudioPluginNode.hpp"
#include "uapmd/priv/plugingraph/AudioPluginHostingAPI.hpp"
#include "remidy-tooling/PluginScanTool.hpp"
#include "remidy-tooling/PluginInstancing.hpp"

namespace uapmd {

    class RemidyAudioPluginHostPAL : public AudioPluginHostingAPI {
        remidy_tooling::PluginScanTool scanning{};
    public:
        RemidyAudioPluginHostPAL();
        ~RemidyAudioPluginHostPAL() override = default;
        remidy::PluginCatalog& catalog() override { return scanning.catalog; }
        void performPluginScanning(bool rescan) override;
        void createPluginInstance(uint32_t sampleRate, uint32_t inputChannels, uint32_t outputChannels, bool offlineMode, std::string &format, std::string &pluginId, std::function<void(std::unique_ptr<AudioPluginNode> node, std::string error)>&& callback) override;
        uapmd_status_t processAudio(std::vector<remidy::AudioProcessContext*> contexts) override;
    };

}
