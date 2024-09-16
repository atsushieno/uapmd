#pragma once
#include "AudioDeviceConfiguration.hpp"

namespace uapmd {

    class VirtualMidiDeviceConfiguration {
        std::string name;

    public:
        VirtualMidiDeviceConfiguration(std::string name, AudioDeviceConfiguration* con);

        const char* getName();
    };

}
