
#include "UapmdMidiDevice.hpp"

UapmdMidiDevice::UapmdMidiDevice(std::string& deviceName, std::string& manufacturer, std::string& version)
    : platformDevice(std::make_unique<PlatformVirtualMidiDevice>(deviceName, manufacturer, version)) {
}
