#pragma once
#include <memory>
#include <remidy/remidy.hpp>
#include "readerwriterqueue.h"
#include "uapmd/priv/midi/MidiIOFeature.hpp"
#include "uapmd/priv/midi/UapmdUmpMapper.hpp"
#include "uapmd/priv/plugin-api/AudioPluginInstanceAPI.hpp"

namespace uapmd {
    class UapmdNodeUmpInputMapper :
        public UapmdUmpInputMapper,
        public remidy::UmpInputDispatcher {
        AudioPluginInstanceAPI* plugin;
        moodycamel::ReaderWriterQueue<uint32_t> preset_load_queue_{4};

    public:
        explicit UapmdNodeUmpInputMapper(AudioPluginInstanceAPI* plugin);

        void process(remidy::AudioProcessContext& src) override;

        void setParameterValue(uint16_t index, double value) override;

        double getParameterValue(uint16_t index) override;

        void setPerNoteControllerValue(uint8_t note, uint8_t index, double value) override;

        // Enqueues the preset load request for later drain on the non-RT thread.
        void loadPreset(uint32_t index) override;

        // Drains queued preset load requests; call from non-RT context only.
        void drainPresetRequests();
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
        ~UapmdNodeUmpOutputMapper() override;

        void sendParameterValue(uint16_t index, double value) override;
        void sendPerNoteControllerValue(uint8_t note, uint8_t index, double value) override;

        void sendPresetIndexChange(uint32_t index) override;
    };
}
