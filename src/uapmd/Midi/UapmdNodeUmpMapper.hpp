#pragma once
#include <memory>
#include <remidy/remidy.hpp>

#include "uapmd/uapmd.hpp"

namespace uapmd {
    class UapmdNodeUmpInputMapper :
        public UapmdUmpInputMapper,
        public remidy::UmpInputDispatcher {
        AudioPluginNodeAPI* plugin;

    public:
        explicit UapmdNodeUmpInputMapper(AudioPluginNodeAPI* plugin);

        void process(uint64_t timestamp, remidy::AudioProcessContext& src) override;

        void setParameterValue(uint16_t index, double value) override;

        double getParameterValue(uint16_t index) override;

        void setPerNoteControllerValue(uint8_t note, uint8_t index, double value) override;

        void loadPreset(uint32_t index) override;
    };

    class UapmdNodeUmpOutputMapper : public UapmdUmpOutputMapper {
        std::shared_ptr<MidiIODevice> device;
        AudioPluginNodeAPI* plugin;
        remidy::PluginParameterSupport::ParameterChangeListenerId param_change_listener_id;

    public:
        explicit UapmdNodeUmpOutputMapper(std::shared_ptr<MidiIODevice> device, AudioPluginNodeAPI* plugin);
        ~UapmdNodeUmpOutputMapper() override;

        void onParameterValueUpdated(uint16_t index, double value) override;

        void onPresetLoaded(uint32_t index) override;
    };
}
