
#include "PlatformVirtualMidiDevice.hpp"
#include "impl/PlatformVirtualMidiDeviceImpl.hpp"

namespace uapmd {

    PlatformVirtualMidiDevice::PlatformVirtualMidiDevice(std::string& deviceName, std::string& manufacturer, std::string& version)
        : impl(new Impl(deviceName, manufacturer, version)) {
    }

    PlatformVirtualMidiDevice::~PlatformVirtualMidiDevice() {
        delete impl;
    }

    void PlatformVirtualMidiDevice::addInputHandler(ump_receiver_t receiver) {
        impl->addInputHandler(receiver);
    }

    void PlatformVirtualMidiDevice::removeInputHandler(ump_receiver_t receiver) {
        impl->removeInputHandler(receiver);
    }

    void PlatformVirtualMidiDevice::send(uapmd_ump_t *messages, size_t length, uapmd_timestamp_t timestamp) {
        impl->send(messages, length, timestamp);
    }

}
