#pragma once

#include "midicci/midicci.hpp"

namespace uapmd {
    class UapmdFunctionBlock;

    // Handles UAPMD-specific MIDI-CI messages. Namely, it registers property getter and setter for
    // AllCtrlList, CtrlMapList, ProgramList, and State.
    //
    // There is no "output interceptor" as it will be handled by MidiCISession and its registered `MidiIODevice`.
    class UapmdMidiCISession {
        UapmdFunctionBlock* device;
        AudioPluginInstanceAPI* instance;

        std::string device_name{};
        std::string manufacturer{};
        std::string version{};

        std::unique_ptr<midicci::musicdevice::MidiCISession> ci_session{};

        std::vector<midicci::musicdevice::MidiInputCallback> ci_input_forwarders{};

        std::function<void()> on_process_midi_message_report{};

    public:
        UapmdMidiCISession(
            UapmdFunctionBlock* device,
            AudioPluginInstanceAPI* instance,
            std::string deviceName,
            std::string manufacturerName,
            std::string versionString
        ) : device(device),
            instance(instance),
            device_name(std::move(deviceName)),
            manufacturer(std::move(manufacturerName)),
            version(std::move(versionString)) {
        }

        void setupMidiCISession();

        void interceptUmpInput(uapmd_ump_t* ump, size_t sizeInBytes, uapmd_timestamp_t timestamp);

        void setMidiMessageReportHandler(std::function<void()>&& onProcessMidiMessageReport) {
            on_process_midi_message_report = onProcessMidiMessageReport;
        }
    };
}