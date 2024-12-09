
#include "UapmdMidiDevice.hpp"

namespace uapmd {

    UapmdMidiDevice::UapmdMidiDevice(std::string& deviceName, std::string& manufacturer, std::string& version) :
        platformDevice(new PlatformVirtualMidiDevice(deviceName, manufacturer, version)),
        // FIXME: do we need valid sampleRate here?
        audioPluginHost(new AudioPluginSequencer(44100, AudioPluginHostPAL::instance())) {
    }

}
