#pragma once

#include <functional>
#include "../plugin-api/AudioPluginFormat.hpp"
#include "../plugin-api/AudioPluginInstanceAPI.hpp"

namespace uapmd_plugin_hosting {
    // a Plugin hosting API Abstraction Layer.
    class AudioPluginHostingAPI {
    public:
        virtual ~AudioPluginHostingAPI() = default;
        virtual std::vector<AudioPluginCatalogEntry> pluginCatalogEntries() = 0;
        virtual void savePluginCatalogToFile(std::filesystem::path path) = 0;
        virtual void performPluginScanning(bool rescan) = 0;
        virtual void reloadPluginCatalogFromCache() = 0;
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

        // Adds a plugin format of the application's own, such as one built on an external
        // plugin engine. The caller keeps ownership and must keep the format alive for as
        // long as this host is used. Its plugins then join the plugin catalog and can be
        // instantiated by name like any built-in format's.
        // Register formats before scanning, so that the first scan picks them up.
        virtual void addPluginFormat(AudioPluginFormat* format) = 0;
        // Every registered format, built-in ones included.
        virtual std::vector<AudioPluginFormat*> pluginFormats() = 0;

        // Reports, by instance ID, that a plugin changed its own internal state, so that the
        // document models can mark it dirty. The host collects these from the instances that
        // register PluginStateChangeExtension; most formats do not, because the host is the
        // single source of truth for their state.
        virtual remidy::EventListenerId addPluginStateChangeListener(std::function<void(int32_t)> listener) = 0;
        virtual void removePluginStateChangeListener(remidy::EventListenerId listenerId) = 0;

        virtual void onTrackGraphNodeAdded(int32_t, int32_t, bool, uint32_t) {}

        virtual std::vector<int32_t> instanceIds() = 0;

        static std::unique_ptr<AudioPluginHostingAPI> create();
    };

}
