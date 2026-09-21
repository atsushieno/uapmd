#pragma once
#include <memory>
#include <remidy/remidy.hpp>
#include "readerwriterqueue.h"
#include <atomic>
#include "uapmd-midi-service/uapmd-midi-service.hpp"
#include "uapmd-plugin-hosting/uapmd-plugin-hosting.hpp"

using namespace uapmd_plugin_hosting;

namespace uapmd_midi_service {
    class UapmdNodeUmpInputMapper :
        public UapmdUmpInputMapper,
        public remidy::UmpInputDispatcher {
        uapmd_plugin_hosting::AudioPluginInstanceAPI* plugin;
        moodycamel::ReaderWriterQueue<uint32_t> preset_load_queue_{64};
        std::atomic<uint32_t> dropped_preset_requests_{0};

    public:
        explicit UapmdNodeUmpInputMapper(uapmd_plugin_hosting::AudioPluginInstanceAPI* plugin);

        void process(remidy::AudioProcessContext& src) override;

        void setParameterValue(uint16_t index, double value) override;

        double getParameterValue(uint16_t index) override;

        void setPerNoteControllerValue(uint8_t note, uint8_t index, double value) override;

        // Captures a request without calling the plugin; newest requests drop on overflow.
        void loadPreset(uint32_t index) override;

        bool tryDequeuePresetRequest(uint32_t& index) override;
        uint32_t droppedPresetRequestCount() const override {
            return dropped_preset_requests_.load(std::memory_order_relaxed);
        }
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
