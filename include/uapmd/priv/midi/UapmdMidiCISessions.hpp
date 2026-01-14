#pragma once

#include "midicci/midicci.hpp"

namespace uapmd {
    class UapmdMidiDevice;

    // Handles UAPMD-specific MIDI-CI messages. Namely, it registers property getter and setter for
    // AllCtrlList, CtrlMapList, ProgramList, and State.
    //
    // There is no "output interceptor" as it will be handled by MidiCISession and its registered `MidiIODevice`.
    class UapmdMidiCISessions {
        UapmdMidiDevice* device;
        SequencerFeature* sequencer;

        std::string device_name{};
        std::string manufacturer{};
        std::string version{};

        std::map<uint32_t, std::unique_ptr<midicci::musicdevice::MidiCISession>> ci_sessions{};

        std::vector<midicci::musicdevice::MidiInputCallback> ci_input_forwarders{};

    public:
        UapmdMidiCISessions(
            UapmdMidiDevice* device,
            SequencerFeature* sharedSequencer,
            std::string deviceName,
            std::string manufacturerName,
            std::string versionString
        ) : device(device),
            sequencer(sharedSequencer),
            device_name(std::move(deviceName)),
            manufacturer(std::move(manufacturerName)),
            version(std::move(versionString)) {
        }

        void setupMidiCISession();

        void interceptUmpInput(uapmd_ump_t* ump, size_t sizeInBytes, uapmd_timestamp_t timestamp);
    };
}