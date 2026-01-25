#include "remidy.hpp"
#include <umppi/umppi.hpp>

namespace {
inline uint8_t midi2NoteAttributeType(const umppi::Ump& ump) {
    return static_cast<uint8_t>(ump.int1 & 0xFFu);
}

inline uint16_t midi2NoteAttributeData(const umppi::Ump& ump) {
    return static_cast<uint16_t>(ump.int2 & 0xFFFFu);
}
}

void remidy::TypedUmpInputDispatcher::process(uint64_t newTimestamp, AudioProcessContext &src) {
    _timestamp = newTimestamp;
    track_context = src.trackContext();

    onProcessStart(src);

    auto* ptr = static_cast<const uint8_t*>(src.eventIn().getMessages());
    auto numBytes = src.eventIn().position();
    size_t offset = 0;
    while (offset + sizeof(uint32_t) <= numBytes) {
        auto* words = reinterpret_cast<const uint32_t*>(ptr + offset);
        const auto messageType = static_cast<uint8_t>(words[0] >> 28);
        const auto wordCount = umppi::umpSizeInInts(messageType);
        const auto messageSize = static_cast<size_t>(wordCount) * sizeof(uint32_t);
        if (offset + messageSize > numBytes)
            break;
        umppi::Ump ump(words[0],
                       wordCount > 1 ? words[1] : 0,
                       wordCount > 2 ? words[2] : 0,
                       wordCount > 3 ? words[3] : 0);
        uint7_t group, channel, note;
        bool relative;
        switch (ump.getMessageType()) {
            case umppi::MessageType::UTILITY:
                switch (static_cast<uint8_t>(ump.getStatusCode())) {
                    case umppi::MidiUtilityStatus::DCTPQ:
                        track_context->deltaClockstampTicksPerQuarterNotes(ump.getDCTPQ());
                        break;
                    case umppi::MidiUtilityStatus::JR_TIMESTAMP:
                        // FIXME: convert to sample-based timestamp (here?)
                        _timestamp += ump.getJRTimestamp();
                        break;
                    case umppi::MidiUtilityStatus::DELTA_CLOCKSTAMP:
                        // FIXME: implement (calculate timestamp delta)
                        break;
                }
                break;
            case umppi::MessageType::MIDI2:
                group = ump.getGroup();
                channel = ump.getChannelInGroup();
                note = -1;
                relative = false;
                switch (static_cast<uint8_t>(ump.getStatusCode())) {
                    case umppi::MidiChannelStatus::NOTE_ON:
                        onNoteOn(group, channel,
                                 ump.getMidi2Note(),
                                 midi2NoteAttributeType(ump),
                                 ump.getMidi2Velocity16(),
                                 midi2NoteAttributeData(ump));
                        break;
                    case umppi::MidiChannelStatus::NOTE_OFF:
                        onNoteOff(group, channel,
                                  ump.getMidi2Note(),
                                  midi2NoteAttributeType(ump),
                                  ump.getMidi2Velocity16(),
                                  midi2NoteAttributeData(ump));
                        break;
                    case umppi::MidiChannelStatus::PAF:
                        note = ump.getMidi2Note();
                    case umppi::MidiChannelStatus::CAF:
                        onPressure(group, channel, note, ump.getMidi2PafData());
                        break;
                    case umppi::MidiChannelStatus::CC:
                        onCC(group, channel,
                             ump.getMidi2CcIndex(),
                             ump.getMidi2CcData());
                        break;
                    case umppi::MidiChannelStatus::PROGRAM:
                        onProgramChange(group, channel,
                                        ump.getMidi2ProgramOptions(),
                                        ump.getMidi2ProgramProgram(),
                                        ump.getMidi2ProgramBankMsb(),
                                        ump.getMidi2ProgramBankLsb());
                        break;
                    case umppi::MidiChannelStatus::PER_NOTE_PITCH_BEND:
                        note = ump.getMidi2Note();
                    case umppi::MidiChannelStatus::PITCH_BEND:
                        onPitchBend(group, channel, note, ump.getMidi2PitchBendData());
                        break;
                    case umppi::MidiChannelStatus::PER_NOTE_RCC:
                        onPNRC(group, channel,
                               ump.getMidi2Note(),
                               static_cast<uint8_t>(ump.int1 & 0xFFu),
                               ump.int2);
                    case umppi::MidiChannelStatus::PER_NOTE_ACC:
                        onPNAC(group, channel,
                               ump.getMidi2Note(),
                               static_cast<uint8_t>(ump.int1 & 0xFFu),
                               ump.int2);
                        break;
                    case umppi::MidiChannelStatus::RELATIVE_RPN:
                        relative = true;
                    case umppi::MidiChannelStatus::RPN:
                        onRC(group, channel,
                             ump.getMidi2RpnMsb(),
                             ump.getMidi2RpnLsb(),
                             ump.getMidi2RpnData(),
                             relative);
                        break;
                    case umppi::MidiChannelStatus::RELATIVE_NRPN:
                        relative = true;
                    case umppi::MidiChannelStatus::NRPN:
                        onAC(group, channel,
                             ump.getMidi2NrpnMsb(),
                             ump.getMidi2NrpnLsb(),
                             ump.getMidi2NrpnData(),
                             relative);
                        break;
                    case umppi::MidiChannelStatus::PER_NOTE_MANAGEMENT:
                        onPerNoteManagement(group, channel,
                                // FIXME: we should not need this cast
                                            static_cast<uint7_t>(ump.getMidi2Note()),
                                            static_cast<uint8_t>(ump.int1 & 0xFFu));
                        break;
                }
                break;
        }
        offset += messageSize;
    }

    onProcessEnd(src);

    track_context = nullptr;
}
