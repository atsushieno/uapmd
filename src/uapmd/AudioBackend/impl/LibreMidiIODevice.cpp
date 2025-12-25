#include "LibreMidiIODevice.hpp"

#include <algorithm>
#include <chrono>
#include <thread>
#include <libremidi/libremidi.hpp>

namespace uapmd {

    LibreMidiIODevice::LibreMidiIODevice(std::string apiName, std::string deviceName, std::string manufacturer, std::string version, uint64_t sysexDelayInMicroseconds)
        : api_name(std::move(apiName)),
          device_name(std::move(deviceName)),
          manufacturer(std::move(manufacturer)),
          version(std::move(version)),
          sysex_delay_in_microseconds(sysexDelayInMicroseconds) {

        assert(libremidi_midi_api_configuration_init(&apiCfg) == 0);
        auto apis = libremidi::available_ump_apis();
        if (remidy_strcasecmp(api_name.c_str(), "PIPEWIRE") == 0)
            apiCfg.api = PIPEWIRE_UMP;
        else if (remidy_strcasecmp(api_name.c_str(), "ALSA") == 0)
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
        midiCfg.port_name = device_name.c_str();
        midiCfg.on_midi2_message.context = this;
        midiCfg.on_midi2_message.callback = midi2_in_callback;
        assert(libremidi_midi_in_new(&midiCfg, &apiCfg, &midiIn) == 0);
        assert(libremidi_midi_out_new(&midiCfg, &apiCfg, &midiOut) == 0);
    }

    LibreMidiIODevice::~LibreMidiIODevice() {
        assert(libremidi_midi_in_free(midiIn) == 0);
        assert(libremidi_midi_out_free(midiOut) == 0);
    }

    void LibreMidiIODevice::midi2_in_callback(void* ctx, libremidi_timestamp timestamp, const libremidi_midi2_symbol* messages, size_t len) {
        static_cast<LibreMidiIODevice*>(ctx)->inputCallback(timestamp, messages, len);
    }

    void LibreMidiIODevice::inputCallback(libremidi_timestamp timestamp, const libremidi_midi2_symbol* messages, size_t len) {
        (void) timestamp;
        for (size_t i = 0, n = receivers.size(); i < n; i++)
            receivers[i](receiver_user_data[i], const_cast<uapmd_ump_t*>(messages), len * sizeof(int32_t), 0);
    }

    void LibreMidiIODevice::addInputHandler(ump_receiver_t receiver, void* userData) {
        receivers.emplace_back(receiver);
        receiver_user_data.emplace_back(userData);
    }

    void LibreMidiIODevice::removeInputHandler(ump_receiver_t receiver) {
        auto pos = std::find(receivers.begin(), receivers.end(), receiver);
        if (pos == receivers.end())
            return;
        auto index = static_cast<size_t>(pos - receivers.begin());
        receivers.erase(pos);
        receiver_user_data.erase(receiver_user_data.begin() + static_cast<std::ptrdiff_t>(index));
    }

    void LibreMidiIODevice::send(uapmd_ump_t* messages, size_t sizeInBytes, uapmd_timestamp_t timestamp) {
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

    uapmd_status_t LibreMidiIODevice::start() {
        return 0;
    }

    uapmd_status_t LibreMidiIODevice::stop() {
        return 0;
    }

}
