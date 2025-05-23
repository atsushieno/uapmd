
#include "VirtualMidiDeviceController.hpp"

namespace uapmd {

    std::unique_ptr<UapmdMidiDevice> VirtualMidiDeviceController::createDevice(
        std::string apiName, std::string deviceName, std::string manufacturer, std::string version) {
        return std::make_unique<UapmdMidiDevice>(apiName, deviceName, manufacturer, version);
    }

    uapmd_status_t VirtualMidiDeviceController::registerPluginChannels(uapmd_device_t*device, std::string pluginID) {
        return -1;
    }

    uapmd_status_t VirtualMidiDeviceController::mapPluginChannel(uapmd_device_t*device, std::string pluginID,
        uint8_t group, uint8_t channel) {
        return -1;
    }

}
