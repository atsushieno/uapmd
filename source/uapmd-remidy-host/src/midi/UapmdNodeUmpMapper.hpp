#pragma once
#include <memory>
#include <remidy/remidy.hpp>
#include "uapmd/priv/plugin-api/PluginSupportView.hpp"

namespace uapmd {
    class UapmdNodeUmpInputMapper :
        public UapmdUmpInputMapper,
        public remidy::UmpInputDispatcher {
        AudioPluginInstanceAPI* plugin;

    public:
        explicit UapmdNodeUmpInputMapper(AudioPluginInstanceAPI* plugin);

        void process(remidy::AudioProcessContext& src) override;

        void setParameterValue(uint16_t index, double value) override;

        double getParameterValue(uint16_t index) override;

        void setPerNoteControllerValue(uint8_t note, uint8_t index, double value) override;

        void loadPreset(uint32_t index) override;
    };

    class UapmdNodeUmpOutputMapper : public UapmdUmpOutputMapper {
        MidiIOFeature* device;
        AudioPluginInstanceAPI* plugin;
        ParameterSupportView parameter_support_;
        ParameterListenerId param_change_listener_id;
        ParameterListenerId per_note_change_listener_id;

        double normalizeParameterValue(uint16_t index, double plainValue) const;
        double normalizePerNoteControllerValue(PerNoteContextFlags types, PerNoteContext context, uint32_t parameterIndex, double plainValue) const;

    public:
        explicit UapmdNodeUmpOutputMapper(MidiIOFeature* device, AudioPluginInstanceAPI* plugin);
        ~UapmdNodeUmpOutputMapper() override;

        void sendParameterValue(uint16_t index, double value) override;
        void sendPerNoteControllerValue(uint8_t note, uint8_t index, double value) override;

        void sendPresetIndexChange(uint32_t index) override;
    };
}
