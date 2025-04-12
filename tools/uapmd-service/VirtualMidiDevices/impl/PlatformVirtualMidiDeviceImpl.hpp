#pragma once

#include <vector>

#include "../PlatformVirtualMidiDevice.hpp"
#include <libremidi/libremidi-c.h>

namespace uapmd {

    // A PAL to virtual MIDI device
    class PlatformVirtualMidiDevice::Impl {
        std::string deviceName;
        std::string manufacturer;
        std::string version;
        std::vector<ump_receiver_t> receivers;
        std::vector<void*> receiver_user_data;

        libremidi_observer_configuration obsCfg{};
        libremidi_api_configuration apiCfg{};
        libremidi_midi_configuration midiCfg{};
        libremidi_midi_in_handle* midiIn{};
        libremidi_midi_out_handle* midiOut{};

        static void midi2_in_callback(void* ctx, libremidi_timestamp timestamp, const libremidi_midi2_symbol* messages, size_t len);
        void inputCallback(libremidi_timestamp timestamp, const libremidi_midi2_symbol* messages, size_t len);

    public:
        Impl(std::string& deviceName, std::string& manufacturer, std::string& version);
        virtual ~Impl();

        void addInputHandler(ump_receiver_t receiver, void* userData);
        void removeInputHandler(ump_receiver_t receiver);
        void send(uapmd_ump_t* messages, size_t length, uapmd_timestamp_t timestamp);
    };

}
