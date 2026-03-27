#pragma once

#include "../CommonTypes.hpp"

namespace uapmd {

    class MidiIOFeature {
    public:
        virtual ~MidiIOFeature() = default;

        virtual void addInputHandler(ump_receiver_t receiver, void* userData) = 0;
        virtual void removeInputHandler(ump_receiver_t receiver) = 0;
        virtual void send(uapmd_ump_t* messages, size_t length, uapmd_timestamp_t timestamp) = 0;
    };

}