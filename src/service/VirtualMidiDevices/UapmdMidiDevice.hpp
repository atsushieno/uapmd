#pragma once
#include <string>

#include "PlatformVirtualMidiDevice.hpp"
#include "../Common/CommonTypes.hpp"
#include "../AudioPluginHosting/AudioPluginHost.hpp"

class UapmdMidiDevice {
    std::unique_ptr<PlatformVirtualMidiDevice> platformDevice;
    uapmd_status_t maybeLoadConfiguration(std::string& portId);
    uapmd_status_t createPluginHost(AudioPluginHost** host);
    uapmd_status_t preparePlugins();

public:
    UapmdMidiDevice(std::string& deviceName, std::string& manufacturer, std::string& version);

    uapmd_status_t openPort(std::string portId);
    uapmd_status_t closePort(std::string portId);
};
