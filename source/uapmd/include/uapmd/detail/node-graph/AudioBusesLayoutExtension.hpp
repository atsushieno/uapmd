#pragma once

#include <cstdint>

#include "AudioGraphExtension.hpp"

namespace uapmd {

    struct AudioGraphBusesLayout {
        uint32_t audio_input_bus_count{1};
        uint32_t audio_output_bus_count{1};
        uint32_t event_input_bus_count{1};
        uint32_t event_output_bus_count{1};
    };

    class AudioBusesLayoutExtension : public AudioGraphExtension {
    public:
        ~AudioBusesLayoutExtension() override = default;

        virtual AudioGraphBusesLayout busesLayout() = 0;
        virtual void applyBusesLayout(const AudioGraphBusesLayout& layout) = 0;
    };

} // namespace uapmd
