#pragma once

#include <functional>
#include <string>
#include <vector>
#include <cstdint>
#include "ParameterTypes.hpp"
#include "PluginSupportView.hpp"
#include "../CommonTypes.hpp"
#include "../midi/UapmdUmpMapper.hpp"
#include "../midi/MidiIOFeature.hpp"

namespace uapmd {

    class AudioPluginInstanceAPI {
    public:
        virtual ~AudioPluginInstanceAPI() = default;
        virtual std::string& displayName() const = 0;
        virtual std::string& formatName() const = 0;
        virtual std::string& pluginId() const = 0;
        virtual bool bypassed() const = 0;
        virtual void bypassed(bool value) = 0;
        virtual uapmd_status_t startProcessing() = 0;
        virtual uapmd_status_t stopProcessing() = 0;
        virtual uapmd_status_t processAudio(AudioProcessContext &process) = 0;
        virtual bool requiresReplacingProcess() const = 0;
        virtual std::vector<ParameterMetadata> parameterMetadataList() = 0;
        virtual std::vector<ParameterMetadata> perNoteControllerMetadataList(PerNoteContextFlags contextFlags, PerNoteContext context) = 0;
        virtual std::vector<PresetsMetadata> presetMetadataList() = 0;
        virtual void loadPreset(int32_t presetIndex) = 0;

        virtual std::vector<uint8_t> saveState() = 0;
        virtual void loadState(std::vector<uint8_t>& state) = 0;

        virtual double getParameterValue(int32_t index) = 0;
        virtual void setParameterValue(int32_t index, double value) = 0;
        virtual std::string getParameterValueString(int32_t index, double value) = 0;
        virtual void setPerNoteControllerValue(uint8_t note, uint8_t index, double value) = 0;
        virtual std::string getPerNoteControllerValueString(uint8_t note, uint8_t index, double value) = 0;

        virtual bool hasUISupport() = 0;
        virtual bool createUI(bool isFloating, void* parentHandle, std::function<bool(uint32_t, uint32_t)> resizeHandler) = 0;
        virtual void destroyUI() = 0;
        virtual bool showUI() = 0;
        virtual void hideUI() = 0;
        virtual bool isUIVisible() const = 0;
        virtual bool setUISize(uint32_t width, uint32_t height) = 0;
        virtual bool getUISize(uint32_t &width, uint32_t &height) = 0;
        virtual bool canUIResize() = 0;
        virtual ParameterSupportView parameterSupport() const = 0;

        virtual void assignMidiDeviceToPlugin(MidiIOFeature* device) = 0;
        virtual void clearMidiDeviceFromPlugin() = 0;

        virtual AudioBusesView audioBuses() const = 0;
    };

}
