#include "LibreMidiIODevice.hpp"
#include "LibreMidiSupport.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <libremidi/libremidi.hpp>

namespace uapmd {

    LibreMidiIODevice::LibreMidiIODevice(std::string apiName, std::string deviceName, std::string manufacturer, std::string version, uint64_t sysexDelayInMicroseconds)
        : api_name(std::move(apiName)),
          device_name(std::move(deviceName)),
          manufacturer(std::move(manufacturer)),
          version(std::move(version)),
          sysex_delay_in_microseconds(sysexDelayInMicroseconds) {

        if (libremidi_midi_api_configuration_init(&apiCfg) != 0)
            throw std::runtime_error("Failed to initialize libremidi API configuration");
        auto resolvedApi = detail::resolveLibremidiUmpApi(api_name);
        if (!resolvedApi)
            throw std::runtime_error("No MIDI 2.0 backend is available on this system");
        apiCfg.api = *resolvedApi;

        if (libremidi_midi_configuration_init(&midiCfg) != 0)
            throw std::runtime_error("Failed to initialize libremidi MIDI configuration");
        midiCfg.virtual_port = true;
        midiCfg.version = libremidi_midi_configuration::MIDI2;

        // Use distinct names for input and output ports to avoid ALSA/UIs confusion.
        in_port_name = device_name + " In";
        out_port_name = device_name + " Out";

        // Create input with callback and its own port name.
        midiCfg.port_name = in_port_name.c_str();
        midiCfg.on_midi2_message.context = this;
        midiCfg.on_midi2_message.callback = midi2_in_callback;
        int err = libremidi_midi_in_new(&midiCfg, &apiCfg, &midiIn);
        if (err != 0)
            throw std::runtime_error("Failed to create libremidi MIDI input (error " + std::to_string(err) + ")");

        // Create output with a separate config to avoid dangling callback fields.
        libremidi_midi_configuration midiOutCfg = midiCfg;
        midiOutCfg.on_midi2_message = {};
        midiOutCfg.port_name = out_port_name.c_str();
        err = libremidi_midi_out_new(&midiOutCfg, &apiCfg, &midiOut);
        if (err != 0)
            throw std::runtime_error("Failed to create libremidi MIDI output (error " + std::to_string(err) + ")");
    }

    LibreMidiIODevice::~LibreMidiIODevice() {
        if (midiIn)
            libremidi_midi_in_free(midiIn);
        if (midiOut)
            libremidi_midi_out_free(midiOut);
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
