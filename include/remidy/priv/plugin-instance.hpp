#pragma once

#include "../remidy.hpp"

namespace remidy {

    // [flags]
    enum class PluginUIThreadRequirement : uint32_t {
        // AudioUnit and LV2, by default (probably bad behaved plugins can be explicitly marked as dirty = AllNonAudioOperation)
        None = 0,
        InstanceControl = 1,
        Parameters = 2,
        // CLAP and VST3, by default (probably good plugins can be excluded out to switch to None)
        // Strictly speaking, CLAP does not require [main-thread] to everything, but it's close enough to label as everything.
        AllNonAudioOperation = 0xFFFFFFFF
    };

    class AudioBuses {
    public:
        virtual ~AudioBuses() = default;


        // Returns true if there is an event input bus that MIDI inputs that the instance received can be mapped to.
        // In a long term, it should be just a shorthand property for current bus configuration.
        virtual bool hasEventInputs() = 0;
        // Returns true if there is an event output bus whose outcome can be mapped to the instance's MIDI outputs.
        // In a long term, it should be just a shorthand property for current bus configuration.
        virtual bool hasEventOutputs() = 0;

        virtual const std::vector<AudioBusConfiguration*>& audioInputBuses() const = 0;
        virtual const std::vector<AudioBusConfiguration*>& audioOutputBuses() const = 0;

        // It can be implemented by each plugin format class so that it can return arbitrary index.
        // The override must ensure that the returned value is either in range or < 0 when there is no input bus.
        virtual int32_t mainInputBusIndex() { return audioInputBuses().size() > 0 ? 0 : -1; }
        // It can be implemented by each plugin format class so that it can return arbitrary index.
        // The override must ensure that the returned value is either in range or < 0 when there is no output bus.
        virtual int32_t mainOutputBusIndex() { return audioOutputBuses().size() > 0 ? 0 : -1; }
    };

    class PluginInstance {
        PluginCatalogEntry* entry;

    protected:
        explicit PluginInstance(PluginCatalogEntry* entry) : entry(entry) {}

    public:
        struct ConfigurationRequest {
            uint32_t sampleRate{44100};
            uint32_t bufferSizeInSamples{4096};
            bool offlineMode{false};
            AudioContentType dataType{AudioContentType::Float32};
            // In the future we should be able to configure audio buses.
            //std::optional<std::vector<BusConfiguration>> inputBuses{};
            //std::optional<std::vector<BusConfiguration>> outputBuses{};
        };

        virtual ~PluginInstance() = default;

        PluginCatalogEntry* info() { return entry; }

        virtual PluginExtensibility<PluginInstance>* getExtensibility() { return nullptr; }

        virtual PluginUIThreadRequirement requiresUIThreadOn() = 0;

        virtual StatusCode configure(ConfigurationRequest& configuration) = 0;

        virtual StatusCode startProcessing() = 0;

        virtual StatusCode stopProcessing() = 0;

        virtual StatusCode process(AudioProcessContext& process) = 0;

        virtual AudioBuses* audioBuses() = 0;

        virtual PluginParameterSupport* parameters() = 0;
    };

}