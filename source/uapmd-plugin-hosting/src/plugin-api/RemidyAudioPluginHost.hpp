#pragma once
#include <cstdint>
#include <atomic>
#include <memory>

#include "remidy/remidy.hpp"
#include "uapmd-plugin-hosting/uapmd-plugin-hosting.hpp"

namespace uapmd_plugin_hosting {

    class RemidyAudioPluginHost : public uapmd_plugin_hosting::AudioPluginHostingAPI {
        std::unique_ptr<uapmd_plugin_hosting::PluginScanTool> scanning;
        std::map<int32_t,std::unique_ptr<uapmd_plugin_hosting::AudioPluginInstanceAPI>> instances{};
        remidy::ParameterEventBase<void, int32_t> plugin_state_change_event_{};
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
        remidy::EventListenerId addPluginStateChangeListener(std::function<void(int32_t)> listener) override;
        void removePluginStateChangeListener(remidy::EventListenerId listenerId) override;
        uapmd_plugin_hosting::AudioPluginInstanceAPI* getInstance(int32_t instanceId) override;
        void onTrackGraphNodeAdded(int32_t instanceId, int32_t trackIndex, bool isMasterTrack, uint32_t order) override;
    };

}
