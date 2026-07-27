#include <umppi/umppi.hpp>
#include "uapmd/uapmd.hpp"

namespace uapmd {
    void UapmdUmpInputMapper::process(remidy::AudioProcessContext& src) {
        auto& inEvents = src.eventIn();
        auto* bytes = static_cast<uint8_t*>(inEvents.getMessages());
        size_t bytesAvailable = inEvents.position();
        size_t offset = 0;
        size_t writeOffset = 0;
        while (offset + sizeof(uint32_t) <= bytesAvailable) {
            const auto* words = reinterpret_cast<const uint32_t*>(bytes + offset);
            auto messageType = static_cast<uint8_t>(words[0] >> 28);
            auto wordCount = umppi::umpSizeInInts(messageType);
            size_t messageSize = static_cast<size_t>(wordCount) * sizeof(uint32_t);
            if (offset + messageSize > bytesAvailable)
                break;
            umppi::Ump ump(words[0],
                           wordCount > 1 ? words[1] : 0,
                           wordCount > 2 ? words[2] : 0,
                           wordCount > 3 ? words[3] : 0);
            bool consumed = false;
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
                        const auto options = ump.getMidi2ProgramOptions();
                        const auto bankMsb = ump.getMidi2ProgramBankMsb();
                        const auto bankLsb = ump.getMidi2ProgramBankLsb();
                        const auto program = ump.getMidi2ProgramProgram();
                        uint32_t presetIndex = program;
                        if (options & umppi::MidiProgramChangeOptions::BANK_VALID) {
                            // UAPMD reserves bank-MSB bit 6 for the upper six bits
                            // of an extended preset index. Otherwise the two bank
                            // bytes form the normal 14-bit bank index.
                            const auto bankIndex = (bankMsb & 0x40 ? 0 : bankMsb) * 0x80 + bankLsb;
                            const auto indexInBank = (bankMsb & 0x40 ? bankMsb - 0x40 : 0) * 0x80 + program;
                            presetIndex = bankIndex * 0x80 + indexInBank;
                        }
                        loadPreset(presetIndex);
                        consumed = true;
                        break;
                    }
                    default:
                        break;
                }
            }
            if (!consumed) {
                if (writeOffset != offset)
                    std::memmove(bytes + writeOffset, bytes + offset, messageSize);
                writeOffset += messageSize;
            }
            offset += messageSize;
        }
        inEvents.position(writeOffset);
    }
}
