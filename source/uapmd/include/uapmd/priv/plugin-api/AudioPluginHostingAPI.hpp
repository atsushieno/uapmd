#pragma once

#include <functional>
#include "../plugin-api/AudioPluginInstanceAPI.hpp"

namespace uapmd {
    // a Plugin hosting API Abstraction Layer.
    class AudioPluginHostingAPI {
    public:
        virtual ~AudioPluginHostingAPI() = default;
        virtual std::vector<remidy::PluginCatalogEntry> pluginCatalogEntries() = 0;
        virtual void savePluginCatalogToFile(std::filesystem::path path) = 0;
        virtual void performPluginScanning(bool rescan) = 0;
        virtual void createPluginInstance(uint32_t sampleRate,
                                          uint32_t bufferSize,
                                          std::optional<uint32_t> mainInputChannels,
                                          std::optional<uint32_t> mainOutputChannels,
                                          bool offlineMode,
                                          std::string &format,
                                          std::string &pluginId,
                                          std::function<void(int32_t instanceId, std::string)>&& callback) = 0;
        virtual void deletePluginInstance(int32_t instanceId) = 0;
        virtual AudioPluginInstanceAPI* getInstance(int32_t instanceId) = 0;
        virtual void onTrackGraphNodeAdded(int32_t, int32_t, bool, uint32_t) {}

        virtual std::vector<int32_t> instanceIds() = 0;

        static std::unique_ptr<AudioPluginHostingAPI> create();
    };

}
