
#include "UapmdMidiDevice.hpp"

namespace uapmd {

    UapmdMidiDevice::UapmdMidiDevice(std::string& deviceName, std::string& manufacturer, std::string& version) :
        platformDevice(new PlatformVirtualMidiDevice(deviceName, manufacturer, version)),
        audioPluginHost(new AudioPluginHost()) {
    }

}
