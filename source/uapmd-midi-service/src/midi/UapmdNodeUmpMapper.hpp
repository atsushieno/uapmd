#pragma once
#include <memory>
#include <remidy/remidy.hpp>
#include "uapmd-midi-service/uapmd-midi-service.hpp"
#include "uapmd-plugin-hosting/uapmd-plugin-hosting.hpp"

using namespace uapmd_plugin_hosting;

namespace uapmd_midi_service {
    class UapmdNodeUmpInputMapper : public UapmdUmpInputMapper {
        uapmd_plugin_hosting::AudioPluginInstanceAPI* plugin;
        std::function<void(uint32_t)> preset_request_sink_;

    public:
        UapmdNodeUmpInputMapper(uapmd_plugin_hosting::AudioPluginInstanceAPI* plugin,
                               std::function<void(uint32_t)> presetRequestSink);

        void setParameterValue(uint16_t index, double value) override;

        double getParameterValue(uint16_t index) override;

        void setPerNoteControllerValue(uint8_t note, uint8_t index, double value) override;

        void requestPresetLoad(uint32_t index) override;
    };

    class UapmdNodeUmpOutputMapper : public UapmdUmpOutputMapper {
        MidiIOFeature* device;
        AudioPluginInstanceAPI* plugin;
        remidy::PluginParameterSupport* parameter_support;
        remidy::EventListenerId param_change_listener_id;
        remidy::EventListenerId per_note_change_listener_id;

        double normalizeParameterValue(uint16_t index, double plainValue) const;
        double normalizePerNoteControllerValue(remidy::PerNoteControllerContextTypes types, uint32_t context, uint32_t parameterIndex, double plainValue) const;

    public:
        explicit UapmdNodeUmpOutputMapper(MidiIOFeature* device, AudioPluginInstanceAPI* plugin);
        ~UapmdNodeUmpOutputMapper() noexcept override;

        void detach() noexcept;

        void sendParameterValue(uint16_t index, double value) override;
        void sendPerNoteControllerValue(uint8_t note, uint8_t index, double value) override;

        void sendPresetIndexChange(uint32_t index) override;
    };
}
