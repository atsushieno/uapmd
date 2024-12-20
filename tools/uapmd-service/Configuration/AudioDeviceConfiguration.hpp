#pragma once
#include <string>

namespace uapmd {

    class AudioDeviceConfiguration {
        std::string configName;
        std::string deviceName;

    public:
        AudioDeviceConfiguration(std::string configName, std::string deviceName);

        const char* name();
        const char* audioDeviceName();
    };

}
