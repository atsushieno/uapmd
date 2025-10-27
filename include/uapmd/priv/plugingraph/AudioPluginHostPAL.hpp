#pragma once

#include <functional>
#include "uapmd/priv/CommonTypes.hpp"

namespace uapmd {
    class AudioPluginNode;
    struct ParameterMetadata;
    struct PresetsMetadata;

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
            virtual std::vector<PresetsMetadata> presetMetadataList() = 0;
            virtual void loadPreset(int32_t presetIndex) = 0;

            virtual std::vector<uint8_t> saveState() = 0;
            virtual void loadState(std::vector<uint8_t>& state) = 0;

            virtual void setParameterValue(int32_t index, double value) = 0;

            virtual bool hasUISupport() = 0;
            virtual bool createUI(bool isFloating, void* parentHandle, std::function<bool(uint32_t, uint32_t)> resizeHandler) = 0;
            virtual void destroyUI() = 0;
            virtual bool showUI() = 0;
            virtual void hideUI() = 0;
            virtual bool isUIVisible() const = 0;
            virtual bool setUISize(uint32_t width, uint32_t height) = 0;
            virtual bool getUISize(uint32_t &width, uint32_t &height) = 0;
            virtual bool canUIResize() = 0;
        };

        static AudioPluginHostPAL* instance();
        virtual ~AudioPluginHostPAL() = default;
        virtual PluginCatalog& catalog() = 0;
        virtual void performPluginScanning(bool rescan) = 0;
        virtual void createPluginInstance(uint32_t sampleRate, std::string &format, std::string &pluginId, std::function<void(std::unique_ptr<AudioPluginNode>, std::string)>&& callback) = 0;
        virtual uapmd_status_t processAudio(std::vector<AudioProcessContext*> contexts) = 0;
    };

}
