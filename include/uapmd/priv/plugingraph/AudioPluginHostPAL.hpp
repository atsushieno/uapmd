#pragma once

#include <functional>
#include "AudioPluginNodePAL.hpp"
#include "uapmd/priv/CommonTypes.hpp"

namespace uapmd {
    class AudioPluginNode;
    struct ParameterMetadata;
    struct PresetsMetadata;

    // This PAL is more like a Plugin(-Format) Abstraction Layer rather than Platform Abstraction Layer.
    class AudioPluginHostPAL {
    public:
        static AudioPluginHostPAL* instance();
        virtual ~AudioPluginHostPAL() = default;
        virtual PluginCatalog& catalog() = 0;
        virtual void performPluginScanning(bool rescan) = 0;
        virtual void createPluginInstance(uint32_t sampleRate, uint32_t inputChannels, uint32_t outputChannels, bool offlineMode, std::string &format, std::string &pluginId, std::function<void(std::unique_ptr<AudioPluginNode>, std::string)>&& callback) = 0;
        virtual uapmd_status_t processAudio(std::vector<AudioProcessContext*> contexts) = 0;
    };

}
