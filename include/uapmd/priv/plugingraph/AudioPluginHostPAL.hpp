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

            virtual bool hasUISupport() { return false; }
            virtual bool createUI(bool isFloating) { return false; }
            virtual bool attachUI(void* parentHandle) { return false; }
            virtual bool showUI() { return false; }
            virtual void hideUI() {}
            virtual bool isUIVisible() const { return false; }
            virtual void setUIResizeHandler(std::function<bool(uint32_t, uint32_t)> handler) { (void) handler; }
            virtual bool setUISize(uint32_t width, uint32_t height) { (void) width; (void) height; return false; }
            virtual bool getUISize(uint32_t &width, uint32_t &height) { (void) width; (void) height; return false; }
        };

        static AudioPluginHostPAL* instance();
        virtual ~AudioPluginHostPAL() = default;
        virtual PluginCatalog& catalog() = 0;
        virtual void performPluginScanning(bool rescan) = 0;
        virtual void createPluginInstance(uint32_t sampleRate, std::string &format, std::string &pluginId, std::function<void(std::unique_ptr<AudioPluginNode>, std::string)>&& callback) = 0;
        virtual uapmd_status_t processAudio(std::vector<AudioProcessContext*> contexts) = 0;
    };

}
