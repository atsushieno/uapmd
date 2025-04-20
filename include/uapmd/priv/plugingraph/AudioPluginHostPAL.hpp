#pragma once

#include "uapmd/priv/CommonTypes.hpp"

namespace uapmd {
    class AudioPluginNode;
    struct ParameterMetadata;

    // This PAL is more like a Plugin(-Format) Abstraction Layer rather than Platform Abstraction Layer.
    class AudioPluginHostPAL {
    public:
        class AudioPluginNodePAL {
        public:
            virtual ~AudioPluginNodePAL() = default;
            virtual std::string& formatName() const = 0;
            virtual std::string& pluginId() const = 0;
            virtual uapmd_status_t processAudio(AudioProcessContext &process) = 0;
            virtual std::vector<ParameterMetadata> parameterMetadataList() = 0;
        };

        static AudioPluginHostPAL* instance();
        virtual ~AudioPluginHostPAL() = default;
        virtual PluginCatalog& catalog() = 0;
        virtual void performPluginScanning(bool rescan) = 0;
        virtual void createPluginInstance(uint32_t sampleRate, std::string &format, std::string &pluginId, std::function<void(std::unique_ptr<AudioPluginNode>, std::string)>&& callback) = 0;
        virtual uapmd_status_t processAudio(std::vector<AudioProcessContext*> contexts) = 0;
    };

}