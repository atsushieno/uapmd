#include <umppi/umppi.hpp>
#include "uapmd/uapmd.hpp"

namespace uapmd {
    void UapmdUmpInputMapper::process(remidy::AudioProcessContext& src) {
        auto& inEvents = src.eventIn();
        auto& outEvents = src.eventOut();
        auto* bytes = static_cast<const uint8_t*>(inEvents.getMessages());
        size_t bytesAvailable = inEvents.position();
        size_t offset = 0;
        while (offset + sizeof(uint32_t) <= bytesAvailable) {
            auto* words = reinterpret_cast<const uint32_t*>(bytes + offset);
            auto messageType = static_cast<uint8_t>(words[0] >> 28);
            auto wordCount = umppi::umpSizeInInts(messageType);
            size_t messageSize = static_cast<size_t>(wordCount) * sizeof(uint32_t);
            if (offset + messageSize > bytesAvailable)
                break;
            umppi::Ump ump(words[0],
                           wordCount > 1 ? words[1] : 0,
                           wordCount > 2 ? words[2] : 0,
                           wordCount > 3 ? words[3] : 0);
            if (ump.getMessageType() == umppi::MessageType::MIDI2) {
                bool relative{false};
                switch (static_cast<uint8_t>(ump.getStatusCode())) {
                    case umppi::MidiChannelStatus::RELATIVE_NRPN:
                        relative = true;
                        [[fallthrough]];
                    case umppi::MidiChannelStatus::NRPN: {
                        const auto bank = ump.getMidi2NrpnMsb();
                        const auto index = ump.getMidi2NrpnLsb();
                        const auto data = ump.getMidi2NrpnData();
                        const auto parameterIndex = bank * 0x80 + index;
                        const double value = relative ?
                                getParameterValue(parameterIndex) + static_cast<double>(data) / INT32_MAX :
                                static_cast<double>(data) / UINT32_MAX;
                        setParameterValue(parameterIndex, value);
                        break;
                    }
                    case umppi::MidiChannelStatus::PER_NOTE_ACC: {
                        const auto note = ump.getMidi2Note();
                        const auto index = static_cast<uint8_t>(ump.int1 & 0xFFu);
                        const auto data = ump.int2;
                        const double value = static_cast<double>(data) / UINT32_MAX;
                        setPerNoteControllerValue(note, index, value);
                        break;
                    }
                    case umppi::MidiChannelStatus::PROGRAM: {
                        const auto bankMsb = ump.getMidi2ProgramBankMsb();
                        const auto bankLsb = ump.getMidi2ProgramBankLsb();
                        const auto program = ump.getMidi2ProgramProgram();
                        // We use the 7th. bit of bankMSB to indicate whether the MSB is actually for bank or index.
                        const auto bankIndex = (bankMsb & 0x40 ? 0 : bankMsb) * 0x80 + bankLsb;
                        const auto presetIndex = (bankMsb & 0x40 ? bankMsb - 0x40 : 0) * 0x80 + program;
                        loadPreset(bankIndex * 0x80 + presetIndex);
                        break;
                    }
                    default:
                        break;
                }
            }
            offset += messageSize;
        }
        outEvents.position(0);
    }
}
