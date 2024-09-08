#pragma once
#include "AudioDeviceConfiguration.hpp"

class VirtualMidiDeviceConfiguration {
    std::string name;

public:
    VirtualMidiDeviceConfiguration(std::string name, AudioDeviceConfiguration* con);

    const char* getName();
};
