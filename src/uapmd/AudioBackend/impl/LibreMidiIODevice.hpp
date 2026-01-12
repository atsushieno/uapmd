#pragma once

#include <string>
#include <vector>

#include "uapmd/uapmd.hpp"
#include <libremidi/libremidi-c.h>

namespace uapmd {

    class LibreMidiIODevice : public MidiIODevice {
        std::string api_name;
        std::string device_name;
        std::string manufacturer;
        std::string version;
        uint64_t sysex_delay_in_microseconds;
        std::string in_port_name;
        std::string out_port_name;
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
        LibreMidiIODevice(std::string apiName, std::string deviceName, std::string manufacturer, std::string version, uint64_t sysExDelayInMicroseconds = 1000);
        ~LibreMidiIODevice() override;

        void addInputHandler(ump_receiver_t receiver, void* userData) override;
        void removeInputHandler(ump_receiver_t receiver) override;
        void send(uapmd_ump_t* messages, size_t length, uapmd_timestamp_t timestamp) override;
    };

}
