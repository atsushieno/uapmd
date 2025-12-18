#include "LibreMidiVirtualMidiDevice.hpp"

#include <iostream>
#include <libremidi/libremidi.hpp>

namespace uapmd {

    LibreMidiVirtualMidiDevice::LibreMidiVirtualMidiDevice(std::string apiName, std::string deviceName, std::string manufacturer, std::string version, uint64_t sysexDelayInMicroseconds)
        : api_name(apiName), device_name(deviceName), manufacturer(manufacturer), version(version), sysex_delay_in_microseconds(sysexDelayInMicroseconds) {

        assert(libremidi_midi_api_configuration_init(&apiCfg) == 0);
        auto apis = libremidi::available_ump_apis();
        if (remidy_strcasecmp(apiName.c_str(), "PIPEWIRE") == 0)
            apiCfg.api = PIPEWIRE_UMP;
        else if (remidy_strcasecmp(apiName.c_str(), "ALSA") == 0)
            apiCfg.api = ALSA_SEQ_UMP;
        else {
            for (auto api : apis)
                if (api == PIPEWIRE_UMP)
                    apiCfg.api = api;
            if (apiCfg.api == UNSPECIFIED)
                apiCfg.api = apis.empty() ? UNSPECIFIED : apis[0];
        }
        assert(libremidi_midi_configuration_init(&midiCfg) == 0);
        midiCfg.virtual_port = true;
        midiCfg.version = libremidi_midi_configuration::MIDI2;
        midiCfg.port_name = deviceName.c_str();
        midiCfg.on_midi2_message.context = this;
        midiCfg.on_midi2_message.callback = midi2_in_callback;
        assert(libremidi_midi_in_new(&midiCfg, &apiCfg, &midiIn) == 0);
        assert(libremidi_midi_out_new(&midiCfg, &apiCfg, &midiOut) == 0);
    }

    LibreMidiVirtualMidiDevice::~LibreMidiVirtualMidiDevice() {
        assert(libremidi_midi_in_free(midiIn) == 0);
        assert(libremidi_midi_out_free(midiOut) == 0);
    }

    void LibreMidiVirtualMidiDevice::midi2_in_callback(void* ctx, libremidi_timestamp timestamp, const libremidi_midi2_symbol* messages, size_t len) {
        static_cast<LibreMidiVirtualMidiDevice *>(ctx)->inputCallback(timestamp, messages, len);
    }

    void LibreMidiVirtualMidiDevice::inputCallback(libremidi_timestamp timestamp, const libremidi_midi2_symbol *messages, size_t len) {
        for (size_t i = 0, n = receivers.size(); i < n; i++)
            receivers[i](receiver_user_data[i], const_cast<uapmd_ump_t *>(messages), len * sizeof(int32_t), 0);
    }

    void LibreMidiVirtualMidiDevice::addInputHandler(ump_receiver_t receiver, void* userData) {
        receivers.emplace_back(receiver);
        receiver_user_data.emplace_back(userData);
    }

    void LibreMidiVirtualMidiDevice::removeInputHandler(ump_receiver_t receiver) {
        auto pos = std::find(receivers.begin(), receivers.end(), receiver);
        auto index = pos - receivers.begin();
        receivers.erase(pos);
        receiver_user_data.erase(receiver_user_data.begin() + index);
    }

    void LibreMidiVirtualMidiDevice::send(uapmd_ump_t *messages, size_t sizeInBytes, uapmd_timestamp_t timestamp) {
        auto total = sizeInBytes / sizeof(int32_t);
        size_t current = 0;
        size_t written = 0;
        size_t chunk = 0;
        while (current < total) {
            uint8_t size = 1;
            switch (((uint32_t) messages[current]) >> 28) {
                case 3:
                case 4:
                    size = 2; break;
                case 5:
                case 0xD:
                case 0xF:
                    size = 4; break;
            }
            chunk += size;
            if (chunk >= 256) {
                libremidi_midi_out_schedule_ump(midiOut, timestamp, messages + written, chunk);
                std::this_thread::sleep_for(std::chrono::microseconds(sysex_delay_in_microseconds));
                written += chunk;
                chunk = 0;
            }
            current += size;
        }
        if (written < total)
            libremidi_midi_out_schedule_ump(midiOut, timestamp, messages + written, total - written);
    }

}
