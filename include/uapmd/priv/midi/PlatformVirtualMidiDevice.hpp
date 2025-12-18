#pragma once

#include <cstddef>
#include <string>

#include "uapmd/priv/CommonTypes.hpp"

namespace uapmd {

    typedef void(*ump_receiver_t)(void* context, uapmd_ump_t* ump, size_t sizeInBytes, uapmd_timestamp_t timestamp);

    class PlatformVirtualMidiDevice {
    public:
        virtual ~PlatformVirtualMidiDevice() = default;

        virtual void addInputHandler(ump_receiver_t receiver, void* userData) = 0;
        virtual void removeInputHandler(ump_receiver_t receiver) = 0;
        virtual void send(uapmd_ump_t* messages, size_t length, uapmd_timestamp_t timestamp) = 0;
    };

}
