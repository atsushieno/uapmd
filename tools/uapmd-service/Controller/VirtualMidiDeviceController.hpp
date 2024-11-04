#pragma once

#include <string>

#include "uapmd/uapmd.hpp"
#include "../VirtualMidiDevices/UapmdMidiDevice.hpp"

namespace uapmd {

    class VirtualMidiDeviceController {
    public:
        std::unique_ptr<UapmdMidiDevice> createDevice(std::string deviceName, std::string manufacturer, std::string version);
        uapmd_status_t registerPluginChannels(uapmd_device_t* device, std::string pluginID);
        uapmd_status_t mapPluginChannel(uapmd_device_t* device, std::string pluginID, uint8_t group, uint8_t channel);
    };

}
