#pragma once
#include <cstdint>
#include <atomic>

#include "remidy/remidy.hpp"
#include "remidy-tooling/PluginScanTool.hpp"
#include "uapmd/uapmd.hpp"

namespace uapmd {

    class RemidyAudioPluginHost : public AudioPluginHostingAPI {
        remidy_tooling::PluginScanTool scanning{};
    public:
        RemidyAudioPluginHost();
        ~RemidyAudioPluginHost() override = default;
        remidy::PluginCatalog& catalog() override { return scanning.catalog; }
        void performPluginScanning(bool rescan) override;
        void createPluginInstance(uint32_t sampleRate, uint32_t inputChannels, uint32_t outputChannels, bool offlineMode, std::string &format, std::string &pluginId, std::function<void(std::unique_ptr<AudioPluginInstanceAPI> node, std::string error)>&& callback) override;
        uapmd_status_t processAudio(std::vector<remidy::AudioProcessContext*> contexts) override;
    };

}
