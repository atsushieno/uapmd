#pragma once
#include <cstdint>
#include <atomic>

#include "remidy/remidy.hpp"
#include "remidy-tooling/PluginScanTool.hpp"
#include "uapmd/uapmd.hpp"

namespace uapmd {

    class RemidyAudioPluginHost : public AudioPluginHostingAPI {
        remidy_tooling::PluginScanTool scanning{};
        std::map<int32_t,std::unique_ptr<AudioPluginInstanceAPI>> instances{};

    public:
        static AudioPluginHostingAPI* instance();

        RemidyAudioPluginHost();
        ~RemidyAudioPluginHost() override = default;

        remidy::PluginCatalog& catalog() override { return scanning.catalog; }
        void performPluginScanning(bool rescan) override;
        void createPluginInstance(uint32_t sampleRate, uint32_t inputChannels, uint32_t outputChannels, bool offlineMode, std::string &format, std::string &pluginId, std::function<void(int32_t instanceId, std::string error)>&& callback) override;
        void deletePluginInstance(int32_t instanceId) override;
        AudioPluginInstanceAPI* getInstance(int32_t instanceId) override;
        uapmd_status_t processAudio(std::vector<remidy::AudioProcessContext*> contexts) override;
    };

}
