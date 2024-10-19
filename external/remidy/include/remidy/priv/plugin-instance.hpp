#pragma once

#include "../remidy.hpp"

namespace remidy {

    enum class AudioContentType {
        Float32,
        Float64
    };

    class AudioPluginInstance {
    protected:
        explicit AudioPluginInstance() = default;

    public:
        struct ConfigurationRequest {
            uint32_t sampleRate{48000};
            uint32_t bufferSizeInSamples{4096};
            bool offlineMode{false};
            AudioContentType dataType{AudioContentType::Float32};
            // In the future we should be able to configure audio buses.
            //std::optional<std::vector<BusConfiguration>> inputBuses{};
            //std::optional<std::vector<BusConfiguration>> outputBuses{};
        };

        virtual ~AudioPluginInstance() = default;

        virtual AudioPluginExtensibility<AudioPluginInstance>* getExtensibility() { return nullptr; }

        // Returns true if there is a main audio input bus.
        // In a long term, it should be just a shorthand property for current bus configuration.
        virtual bool hasAudioInputs() = 0;
        // Returns true if there is a main audio output bus.
        // In a long term, it should be just a shorthand property for current bus configuration.
        virtual bool hasAudioOutputs() = 0;
        // Returns true if there is an event input bus that MIDI inputs that the instance received can be mapped to.
        // In a long term, it should be just a shorthand property for current bus configuration.
        virtual bool hasEventInputs() = 0;
        // Returns true if there is an event output bus whose outcome can be mapped to the instance's MIDI outputs.
        // In a long term, it should be just a shorthand property for current bus configuration.
        virtual bool hasEventOutputs() = 0;

        //virtual StatusCode getAudioInputBuses(std::vector<BusConfiguration>& results);
        //virtual StatusCode getAudioOutputBuses(std::vector<BusConfiguration>& results);

        virtual StatusCode configure(ConfigurationRequest& configuration) = 0;

        virtual StatusCode startProcessing() = 0;

        virtual StatusCode stopProcessing() = 0;

        virtual StatusCode process(AudioProcessContext& process) = 0;
    };

}