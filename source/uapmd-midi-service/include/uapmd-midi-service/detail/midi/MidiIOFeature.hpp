#pragma once

#include "uapmd-plugin-hosting/detail/CommonTypes.hpp"

namespace uapmd_midi_service {

    class MidiIOFeature {
    public:
        virtual ~MidiIOFeature() = default;

        virtual void addInputHandler(uapmd::ump_receiver_t receiver, void* userData) = 0;
        virtual void removeInputHandler(uapmd::ump_receiver_t receiver) = 0;
        virtual void send(uapmd_ump_t* messages, size_t length, uapmd_timestamp_t timestamp) = 0;
    };

}