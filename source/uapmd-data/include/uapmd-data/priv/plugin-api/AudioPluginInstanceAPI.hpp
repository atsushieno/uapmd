#pragma once

#include <functional>
#include <string>
#include <vector>
#include <cstdint>
#include "remidy/remidy.hpp"
#include "uapmd/uapmd.hpp"

namespace uapmd {
    class AudioPluginInstanceAPI : public AudioPluginInstanceFeature {
    public:
        ~AudioPluginInstanceAPI() override = default;
        virtual std::string& displayName() const = 0;
        virtual std::string& formatName() const = 0;
        virtual std::string& pluginId() const = 0;
        virtual uapmd_status_t startProcessing() = 0;
        virtual uapmd_status_t stopProcessing() = 0;

        virtual bool requiresReplacingProcess() const = 0;

        virtual std::string getParameterValueString(int32_t index, double value) = 0;
        virtual std::string getPerNoteControllerValueString(uint8_t node, uint8_t index, double value) = 0;

        virtual bool hasUISupport() = 0;
        virtual bool createUI(bool isFloating, void* parentHandle, std::function<bool(uint32_t, uint32_t)> resizeHandler) = 0;
        virtual void destroyUI() = 0;
        virtual bool showUI() = 0;
        virtual void hideUI() = 0;
        virtual bool isUIVisible() const = 0;
        virtual bool setUISize(uint32_t width, uint32_t height) = 0;
        virtual bool getUISize(uint32_t &width, uint32_t &height) = 0;
        virtual bool canUIResize() = 0;

        virtual void assignMidiDeviceToPlugin(MidiIOFeature* device) = 0;
        virtual void clearMidiDeviceFromPlugin() = 0;

        virtual remidy::PluginAudioBuses* audioBuses() = 0;
    };

}
