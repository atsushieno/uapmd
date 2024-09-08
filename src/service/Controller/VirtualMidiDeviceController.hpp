#pragma once

#include <string>

#include "../Common/CommonTypes.hpp"
#include "../VirtualMidiDevices/UapmdMidiDevice.hpp"

class VirtualMidiDeviceController {
public:
    std::unique_ptr<UapmdMidiDevice> createDevice(std::string deviceName, std::string manufacturer, std::string version);
    uapmd_status_t registerPluginChannels(uapmd_device_t* device, std::string pluginID);
    uapmd_status_t mapPluginChannel(uapmd_device_t* device, std::string pluginID, uint8_t group, uint8_t channel);
};

