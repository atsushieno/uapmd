#pragma once
#include <cstdint>
#include <atomic>
#include <memory>

#include "remidy/remidy.hpp"
#include "remidy-tooling/remidy-tooling.hpp"
#include "uapmd/uapmd.hpp"

namespace uapmd {

    class RemidyAudioPluginHost : public AudioPluginHostingAPI {
        std::unique_ptr<remidy_tooling::PluginScanTool> scanning;
        std::map<int32_t,std::unique_ptr<AudioPluginInstanceAPI>> instances{};
#if _WIN32
        bool comInitialized{false};
#endif

    public:
        RemidyAudioPluginHost();
        ~RemidyAudioPluginHost() override;

        std::vector<remidy::PluginCatalogEntry> pluginCatalogEntries() override;
        void savePluginCatalogToFile(std::filesystem::path path) override;
        void performPluginScanning(bool rescan) override;
        void reloadPluginCatalogFromCache() override;
        void createPluginInstance(uint32_t sampleRate,
                                  uint32_t bufferSize,
                                  std::optional<uint32_t> mainInputChannels,
                                  std::optional<uint32_t> mainOutputChannels,
                                  bool offlineMode,
                                  std::string &format,
                                  std::string &pluginId,
                                  std::function<void(int32_t instanceId, std::string error)>&& callback) override;
        void deletePluginInstance(int32_t instanceId) override;
        std::vector<int32_t> instanceIds() override;
        AudioPluginInstanceAPI* getInstance(int32_t instanceId) override;
        void onTrackGraphNodeAdded(int32_t instanceId, int32_t trackIndex, bool isMasterTrack, uint32_t order) override;
    };

}
