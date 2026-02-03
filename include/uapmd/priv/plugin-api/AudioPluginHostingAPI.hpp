#pragma once

#include <functional>
#include "../plugin-api/AudioPluginInstanceAPI.hpp"

namespace uapmd {
    // a Plugin hosting API Abstraction Layer.
    class AudioPluginHostingAPI {
    public:
        virtual ~AudioPluginHostingAPI() = default;
        virtual PluginCatalog& catalog() = 0;
        virtual void performPluginScanning(bool rescan) = 0;
        virtual void createPluginInstance(uint32_t sampleRate, uint32_t inputChannels, uint32_t outputChannels, bool offlineMode, std::string &format, std::string &pluginId, std::function<void(int32_t instanceId, std::string)>&& callback) = 0;
        virtual void deletePluginInstance(int32_t instanceId) = 0;
        virtual AudioPluginInstanceAPI* getInstance(int32_t instanceId) = 0;
        virtual uapmd_status_t processAudio(std::vector<AudioProcessContext*> contexts) = 0;

        virtual std::vector<int32_t> instanceIds() = 0;
    };

}
