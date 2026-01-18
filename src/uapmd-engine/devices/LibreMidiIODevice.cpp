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

        auto resolvedApi = detail::resolveLibremidiUmpApi(api_name);
        if (!resolvedApi)
            throw std::runtime_error("No MIDI 2.0 backend is available on this system");

        // Use distinct names for input and output ports to avoid ALSA/UIs confusion.
        in_port_name = device_name + " In";
        out_port_name = device_name + " Out";

        // Create input with callback
        libremidi::ump_input_configuration inConfig{};
        inConfig.on_message = [this](libremidi::ump&& message) {
            inputCallback(std::move(message));
        };
        inConfig.ignore_sysex = false;

        try {
            midiIn = std::make_unique<libremidi::midi_in>(inConfig, *resolvedApi);
            midiIn->open_virtual_port(in_port_name);
        } catch (const std::exception& e) {
            throw std::runtime_error(std::string("Failed to create libremidi MIDI input: ") + e.what());
        }

        // Create output
        libremidi::output_configuration outConfig{};

        try {
            midiOut = std::make_unique<libremidi::midi_out>(outConfig, *resolvedApi);
            midiOut->open_virtual_port(out_port_name);
        } catch (const std::exception& e) {
            throw std::runtime_error(std::string("Failed to create libremidi MIDI output: ") + e.what());
        }
    }

    LibreMidiIODevice::~LibreMidiIODevice() = default;

    void LibreMidiIODevice::inputCallback(libremidi::ump&& message) {
        for (size_t i = 0, n = receivers.size(); i < n; i++)
            receivers[i](receiver_user_data[i], const_cast<uint32_t*>(message.data), sizeof(message.data), message.timestamp);
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
        auto total = sizeInBytes / sizeof(uint32_t);
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
                midiOut->send_ump(reinterpret_cast<const uint32_t*>(messages + written), chunk);
                std::this_thread::sleep_for(std::chrono::microseconds(sysex_delay_in_microseconds));
                written += chunk;
                chunk = 0;
            }
            current += size;
        }
        if (written < total) {
            midiOut->send_ump(reinterpret_cast<const uint32_t*>(messages + written), total - written);
        }
    }
}
