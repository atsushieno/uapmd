#pragma once

#include "uapmd/uapmd.hpp"

namespace uapmd {
    class UapmdMidiCISessions {
        UapmdMidiDevice* device;

        std::string device_name{};
        std::string manufacturer{};
        std::string version{};

        std::map<uint32_t, std::unique_ptr<midicci::musicdevice::MidiCISession>> ci_sessions{};

        std::vector<midicci::musicdevice::MidiInputCallback> ci_input_forwarders{};

    public:
        UapmdMidiCISessions(
            UapmdMidiDevice* device,
            std::string deviceName,
            std::string manufacturerName,
            std::string versionString
        ) : device(device),
            device_name(std::move(deviceName)),
            manufacturer(std::move(manufacturerName)),
            version(std::move(versionString)) {
        }

        void setupMidiCISession();

        void interceptUmpInput(uapmd_ump_t* ump, size_t sizeInBytes, uapmd_timestamp_t timestamp);
    };
}