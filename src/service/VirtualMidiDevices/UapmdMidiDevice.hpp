#pragma once
#include <string>

#include "PlatformVirtualMidiDevice.hpp"
#include "../Common/CommonTypes.hpp"
#include "../AudioPluginHosting/AudioPluginHost.hpp"

namespace uapmd {

    class UapmdMidiDevice {
        std::unique_ptr<PlatformVirtualMidiDevice> platformDevice;
        std::unique_ptr<AudioPluginHost> audioPluginHost;
        uapmd_status_t maybeLoadConfiguration(std::string& portId);
        uapmd_status_t preparePlugins();

    public:
        UapmdMidiDevice(std::string& deviceName, std::string& manufacturer, std::string& version);

        uapmd_status_t openPort(std::string portId);
        uapmd_status_t closePort(std::string portId);
    };

}
