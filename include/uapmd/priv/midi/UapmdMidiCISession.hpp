#pragma once

#include <functional>
#include <memory>
#include <string>

#include "uapmd/priv/CommonTypes.hpp"

namespace uapmd {
    class UapmdFunctionBlock;
    class AudioPluginInstanceAPI;

    // Handles UAPMD-specific MIDI-CI messages. Namely, it registers property getter and setter for
    // AllCtrlList, CtrlMapList, ProgramList, and State.
    //
    // There is no "output interceptor" as it will be handled by MidiCISession and its registered `MidiIODevice`.
    class UapmdMidiCISession {
    public:
        virtual ~UapmdMidiCISession() = default;

        static std::unique_ptr<UapmdMidiCISession> create(
            UapmdFunctionBlock* device,
            AudioPluginInstanceAPI* instance,
            std::string deviceName,
            std::string manufacturerName,
            std::string versionString);

        virtual void setupMidiCISession() = 0;

        virtual void interceptUmpInput(uapmd_ump_t* ump, size_t sizeInBytes, uapmd_timestamp_t timestamp) = 0;

        virtual void setMidiMessageReportHandler(std::function<void()>&& onProcessMidiMessageReport) = 0;
    };
}
