#include <algorithm>
#include <umppi/umppi.hpp>
#include "remidy/remidy.hpp"

namespace {
inline uint8_t midi2NoteAttributeType(const umppi::Ump& ump) {
    return static_cast<uint8_t>(ump.int1 & 0xFFu);
}

inline uint16_t midi2NoteAttributeData(const umppi::Ump& ump) {
    return static_cast<uint16_t>(ump.int2 & 0xFFFFu);
}
}

bool remidy::Sysex7Assembler::feed(uint32_t word0, uint32_t word1, const uint8_t*& data, size_t& size) {
    auto& state = groups[(word0 >> 24) & 0xFu];
    const auto status = static_cast<uint8_t>((word0 >> 16) & 0xF0u);
    const auto count = std::min<uint32_t>((word0 >> 16) & 0xFu, 6);
    const bool starts = status == umppi::Midi2BinaryChunkStatus::COMPLETE_PACKET || status == umppi::Midi2BinaryChunkStatus::START;
    const bool ends = status == umppi::Midi2BinaryChunkStatus::COMPLETE_PACKET || status == umppi::Midi2BinaryChunkStatus::END;
    if (starts) {
        state.size = 0;
        state.overflow = false;
        state.buffer[state.size++] = 0xF0;
    } else if (state.size == 0 || (status != umppi::Midi2BinaryChunkStatus::CONTINUE && !ends))
        return false;

    const uint8_t bytes[6] = {
        static_cast<uint8_t>(word0 >> 8), static_cast<uint8_t>(word0),
        static_cast<uint8_t>(word1 >> 24), static_cast<uint8_t>(word1 >> 16),
        static_cast<uint8_t>(word1 >> 8), static_cast<uint8_t>(word1)};
    // One byte is kept for the trailing F7.
    if (state.size + count + 1 > MAX_MESSAGE_SIZE)
        state.overflow = true;
    else {
        std::copy_n(bytes, count, state.buffer.data() + state.size);
        state.size += count;
    }
    if (!ends)
        return false;

    const bool complete = !state.overflow;
    if (complete) {
        state.buffer[state.size++] = 0xF7;
        data = state.buffer.data();
        size = state.size;
    }
    state.size = 0;
    return complete;
}

void remidy::TypedUmpInputDispatcher::process(AudioProcessContext &src) {
    _jr_timestamp_ticks = 0;
    master_context = &src.masterContext();

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
                    // FIXME: we should make this optional. A sequencer engine might not want have tracks update this value.
                    case umppi::MidiUtilityStatus::DCTPQ:
                        master_context->deltaClockstampTicksPerQuarterNotes(ump.getDCTPQ());
                        break;
                    case umppi::MidiUtilityStatus::JR_TIMESTAMP:
                        _jr_timestamp_ticks += ump.getJRTimestamp();
                        break;
                    case umppi::MidiUtilityStatus::DELTA_CLOCKSTAMP:
                        // FIXME: implement (calculate timestamp delta)
                        break;
                }
                break;
            case umppi::MessageType::SYSEX7: {
                const uint8_t* sysex = nullptr;
                size_t sysexSize = 0;
                if (sysex7_assembler.feed(ump.int1, ump.int2, sysex, sysexSize))
                    onSysEx7(static_cast<uint4_t>(ump.getGroup()), sysex, sysexSize);
                break;
            }
            case umppi::MessageType::MIDI1:
                group = ump.getGroup();
                channel = ump.getChannelInGroup();
                switch (static_cast<uint8_t>(ump.getStatusCode())) {
                    case umppi::MidiChannelStatus::NOTE_OFF:
                        onNoteOff(group, channel, ump.getMidi1Note(), 0, ump.getMidi1Velocity() << 9, 0);
                        break;
                    case umppi::MidiChannelStatus::NOTE_ON:
                        onNoteOn(group, channel, ump.getMidi1Note(), 0, ump.getMidi1Velocity() << 9, 0);
                        break;
                    case umppi::MidiChannelStatus::PAF:
                        onPressure(group, channel, ump.getMidi1Note(), ump.getMidi1CCData() << 25);
                        break;
                    case umppi::MidiChannelStatus::CC:
                        onCC(group, channel, ump.getMidi1CCIndex(), ump.getMidi1CCData() << 25);
                        break;
                    case umppi::MidiChannelStatus::PROGRAM:
                        onProgramChange(group, channel, 0, ump.getMidi1Program(), 0, 0);
                        break;
                    case umppi::MidiChannelStatus::CAF:
                        onPressure(group, channel, -1, ump.getMidi1CCIndex() << 25);
                        break;
                    case umppi::MidiChannelStatus::PITCH_BEND:
                        onPitchBend(group, channel, -1, ump.getMidi1PitchBendData() << 18);
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
                        break;
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

    master_context = nullptr;
}
