#pragma once
#include <memory>
#include "MidiIOFeature.hpp"

namespace uapmd {
    class MidiIOManagerFeature {
    public:
        virtual ~MidiIOManagerFeature() = default;

        virtual std::shared_ptr<MidiIOFeature> createMidiIOFeature(
            std::string apiName, std::string deviceName, std::string manufacturer, std::string version) = 0;
    };
}
