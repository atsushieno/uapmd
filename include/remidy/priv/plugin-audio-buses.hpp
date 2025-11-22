#pragma once

#include <string>
#include <vector>
#include <ranges>

namespace remidy {

    class PluginAudioBuses {
    public:
        virtual ~PluginAudioBuses() = default;


        // Returns true if there is an event input bus that MIDI inputs that the instance received can be mapped to.
        // In the long term, it should be just a shorthand property for the current bus configuration.
        virtual bool hasEventInputs() = 0;
        // Returns true if there is an event output bus whose outcome can be mapped to the instance's MIDI outputs.
        // In the long term, it should be just a shorthand property for the current bus configuration.
        virtual bool hasEventOutputs() = 0;

        virtual const std::vector<AudioBusConfiguration*>& audioInputBuses() const = 0;
        virtual const std::vector<AudioBusConfiguration*>& audioOutputBuses() const = 0;

        // It can be implemented by each plugin format class so that it can return an arbitrary index.
        // The override must ensure that the returned value is either in range or < 0 when there is no input bus.
        virtual int32_t mainInputBusIndex() { return audioInputBuses().size() > 0 ? 0 : -1; }
        // It can be implemented by each plugin format class so that it can return an arbitrary index.
        // The override must ensure that the returned value is either in range or < 0 when there is no output bus.
        virtual int32_t mainOutputBusIndex() { return audioOutputBuses().size() > 0 ? 0 : -1; }
    };
}
