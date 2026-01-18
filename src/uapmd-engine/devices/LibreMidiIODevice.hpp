#pragma once

#include <memory>
#include <string>
#include <vector>

#include <libremidi/libremidi.hpp>
#include "uapmd-engine/uapmd-engine.hpp"

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

        std::unique_ptr<libremidi::midi_in> midiIn;
        std::unique_ptr<libremidi::midi_out> midiOut;

        void inputCallback(libremidi::ump&& message);

    public:
        LibreMidiIODevice(std::string apiName, std::string deviceName, std::string manufacturer, std::string version, uint64_t sysExDelayInMicroseconds = 1000);
        ~LibreMidiIODevice() override;

        void addInputHandler(ump_receiver_t receiver, void* userData) override;
        void removeInputHandler(ump_receiver_t receiver) override;
        void send(uapmd_ump_t* messages, size_t length, uapmd_timestamp_t timestamp) override;
    };

}
