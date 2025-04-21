#pragma once

#include <vector>

#include "uapmd/priv/CommonTypes.hpp"

namespace uapmd {

    typedef void(*ump_receiver_t)(void* context, uapmd_ump_t* ump, size_t sizeInBytes, uapmd_timestamp_t timestamp);

    // A PAL to virtual MIDI device
    class PlatformVirtualMidiDevice {
        class Impl;
        Impl* impl;

    public:
        PlatformVirtualMidiDevice(std::string& deviceName, std::string& manufacturer, std::string& version);
        ~PlatformVirtualMidiDevice();

        void addInputHandler(ump_receiver_t receiver, void* userData);
        void removeInputHandler(ump_receiver_t receiver);
        void send(uapmd_ump_t* messages, size_t length, uapmd_timestamp_t timestamp);
    };

}
