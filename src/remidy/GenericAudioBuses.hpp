#pragma once

namespace remidy {
    class GenericAudioBuses : public PluginAudioBuses {
    protected:
        std::vector<AudioBusDefinition> input_bus_defs{};
        std::vector<AudioBusDefinition> output_bus_defs{};
        std::vector<AudioBusConfiguration*> audio_in_buses{};
        std::vector<AudioBusConfiguration*> audio_out_buses{};

    public:
        struct BusSearchResult {
            uint32_t numAudioIn{0};
            uint32_t numAudioOut{0};
            uint32_t numEventIn{0};
            uint32_t numEventOut{0};
        };
        BusSearchResult busesInfo{};
        virtual void inspectBuses() = 0;

        bool hasEventInputs() override { return busesInfo.numEventIn > 0; }
        bool hasEventOutputs() override { return busesInfo.numEventOut > 0; }

        const std::vector<AudioBusConfiguration*>& audioInputBuses() const override { return audio_in_buses; }
        const std::vector<AudioBusConfiguration*>& audioOutputBuses() const override { return audio_out_buses; }
    };
}
