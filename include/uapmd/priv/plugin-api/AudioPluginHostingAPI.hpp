#pragma once

#include <functional>
#include "../plugin-api/AudioPluginNodeAPI.hpp"
#include "uapmd/priv/CommonTypes.hpp"

namespace uapmd {
    // FIXME: we will replace this with AudioPluginNodeAPI later.
    class AudioPluginNode;

    // This PAL is more like a Plugin hosting API Abstraction Layer rather than a Platform Abstraction Layer.
    class AudioPluginHostingAPI {
    public:
        static AudioPluginHostingAPI* instance();
        virtual ~AudioPluginHostingAPI() = default;
        virtual PluginCatalog& catalog() = 0;
        virtual void performPluginScanning(bool rescan) = 0;
        virtual void createPluginInstance(uint32_t sampleRate, uint32_t inputChannels, uint32_t outputChannels, bool offlineMode, std::string &format, std::string &pluginId, std::function<void(std::unique_ptr<AudioPluginNode>, std::string)>&& callback) = 0;
        virtual uapmd_status_t processAudio(std::vector<AudioProcessContext*> contexts) = 0;
    };

}
