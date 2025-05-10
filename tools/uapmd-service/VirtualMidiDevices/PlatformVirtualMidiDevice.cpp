
#include "PlatformVirtualMidiDevice.hpp"
#include "impl/PlatformVirtualMidiDeviceImpl.hpp"

namespace uapmd {

    PlatformVirtualMidiDevice::PlatformVirtualMidiDevice(std::string& apiName, std::string& deviceName, std::string& manufacturer, std::string& version)
        : impl(new Impl(apiName, deviceName, manufacturer, version)) {
    }

    PlatformVirtualMidiDevice::~PlatformVirtualMidiDevice() {
        delete impl;
    }

    void PlatformVirtualMidiDevice::addInputHandler(ump_receiver_t receiver, void* userData) {
        impl->addInputHandler(receiver, userData);
    }

    void PlatformVirtualMidiDevice::removeInputHandler(ump_receiver_t receiver) {
        impl->removeInputHandler(receiver);
    }

    void PlatformVirtualMidiDevice::send(uapmd_ump_t *messages, size_t length, uapmd_timestamp_t timestamp) {
        impl->send(messages, length, timestamp);
    }
}
