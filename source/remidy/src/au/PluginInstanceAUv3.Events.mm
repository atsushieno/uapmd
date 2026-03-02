#if __APPLE__

#include "PluginFormatAU.hpp"
#include <umppi/umppi.hpp>
#import <CoreMIDI/CoreMIDI.h>

namespace remidy {

// Implementation of MIDIEventConverter - converts UMP to AURenderEvent
AURenderEvent* PluginInstanceAUv3::MIDIEventConverter::convertUMPToRenderEvents(EventSequence& eventIn, AUEventSampleTime eventSampleTime) {
    eventCount = 0;

    if (eventIn.position() == 0)
        return nullptr;

    const uint8_t* umpData = (const uint8_t*)eventIn.getMessages();
    size_t umpByteCount = eventIn.position();

    size_t offset = 0;
    while (offset + sizeof(uint32_t) <= umpByteCount) {
        if (eventCount >= MAX_EVENTS)
            break;

        const uint32_t* words = reinterpret_cast<const uint32_t*>(umpData + offset);
        uint8_t messageType = static_cast<uint8_t>(words[0] >> 28);
        auto wordCount = umppi::umpSizeInInts(messageType);
        size_t messageSize = static_cast<size_t>(wordCount) * sizeof(uint32_t);
        if (offset + messageSize > umpByteCount)
            break;
        umppi::Ump ump(words[0],
                       wordCount > 1 ? words[1] : 0,
                       wordCount > 2 ? words[2] : 0,
                       wordCount > 3 ? words[3] : 0);
        uint8_t group = ump.getGroup();
        uint8_t status = static_cast<uint8_t>(ump.getStatusCode());
        uint8_t channel = ump.getChannelInGroup();

        // Convert MIDI 2.0 channel voice messages to MIDI 1.0 for AURenderEvent
        if (messageType == static_cast<uint8_t>(umppi::MessageType::MIDI2)) {
            AURenderEvent* evt = &eventStorage[eventCount];
            evt->head.eventType = AURenderEventMIDI;
            evt->head.eventSampleTime = eventSampleTime;
            evt->MIDI.cable = group;
            evt->MIDI.length = 0;

            switch (status) {
                case umppi::MidiChannelStatus::NOTE_OFF: {
                    uint8_t note = ump.getMidi2Note();
                    uint16_t velocity16 = ump.getMidi2Velocity16();
                    uint8_t velocity = velocity16 >> 9; // 16-bit to 7-bit

                    evt->MIDI.data[0] = 0x80 | channel;
                    evt->MIDI.data[1] = note;
                    evt->MIDI.data[2] = velocity;
                    evt->MIDI.length = 3;
                    break;
                }

                case umppi::MidiChannelStatus::NOTE_ON: {
                    uint8_t note = ump.getMidi2Note();
                    uint16_t velocity16 = ump.getMidi2Velocity16();
                    uint8_t velocity = velocity16 >> 9;

                    evt->MIDI.data[0] = 0x90 | channel;
                    evt->MIDI.data[1] = note;
                    evt->MIDI.data[2] = velocity;
                    evt->MIDI.length = 3;
                    break;
                }

                case umppi::MidiChannelStatus::PAF: {
                    uint8_t note = ump.getMidi2Note();
                    uint32_t pressure32 = ump.getMidi2PafData();
                    uint8_t pressure = pressure32 >> 25; // 32-bit to 7-bit

                    evt->MIDI.data[0] = 0xA0 | channel;
                    evt->MIDI.data[1] = note;
                    evt->MIDI.data[2] = pressure;
                    evt->MIDI.length = 3;
                    break;
                }

                case umppi::MidiChannelStatus::CC: {
                    uint8_t cc = ump.getMidi2CcIndex();
                    uint32_t value32 = ump.getMidi2CcData();
                    uint8_t value = value32 >> 25; // 32-bit to 7-bit

                    evt->MIDI.data[0] = 0xB0 | channel;
                    evt->MIDI.data[1] = cc;
                    evt->MIDI.data[2] = value;
                    evt->MIDI.length = 3;
                    break;
                }

                case umppi::MidiChannelStatus::PROGRAM: {
                    uint8_t program = ump.getMidi2ProgramProgram();

                    evt->MIDI.data[0] = 0xC0 | channel;
                    evt->MIDI.data[1] = program;
                    evt->MIDI.length = 2;
                    break;
                }

                case umppi::MidiChannelStatus::CAF: {
                    uint32_t pressure32 = ump.getMidi2CafData();
                    uint8_t pressure = pressure32 >> 25;

                    evt->MIDI.data[0] = 0xD0 | channel;
                    evt->MIDI.data[1] = pressure;
                    evt->MIDI.length = 2;
                    break;
                }

                case umppi::MidiChannelStatus::PITCH_BEND: {
                    uint32_t bend32 = ump.getMidi2PitchBendData();
                    uint16_t bend14 = bend32 >> 18; // 32-bit to 14-bit
                    uint8_t lsb = bend14 & 0x7F;
                    uint8_t msb = (bend14 >> 7) & 0x7F;

                    evt->MIDI.data[0] = 0xE0 | channel;
                    evt->MIDI.data[1] = lsb;
                    evt->MIDI.data[2] = msb;
                    evt->MIDI.length = 3;
                    break;
                }

                default:
                    // Unknown/unsupported message type, skip
                    continue;
            }

            // Link events together
            if (eventCount > 0) {
                eventStorage[eventCount - 1].head.next = evt;
            }
            evt->head.next = nullptr;
            eventCount++;
        }
        // Handle MIDI 1.0 channel voice messages
        else if (messageType == static_cast<uint8_t>(umppi::MessageType::MIDI1)) {
            AURenderEvent* evt = &eventStorage[eventCount];
            evt->head.eventType = AURenderEventMIDI;
            evt->head.eventSampleTime = eventSampleTime;
            evt->MIDI.cable = group;

            // MIDI 1.0 messages are already in the right format - just copy the bytes
            uint8_t statusByte = static_cast<uint8_t>((words[0] >> 16) & 0xFF);
            uint8_t statusNibble = statusByte >> 4;
            evt->MIDI.data[0] = statusByte;

            // Extract data bytes from the UMP word
            evt->MIDI.data[1] = (words[0] >> 8) & 0x7F;
            evt->MIDI.data[2] = words[0] & 0x7F;

            // Determine length based on status
            switch (statusNibble) {
                case 0x8: // Note Off
                case 0x9: // Note On
                case 0xA: // Poly Aftertouch
                case 0xB: // Control Change
                case 0xE: // Pitch Bend
                    evt->MIDI.length = 3;
                    break;
                case 0xC: // Program Change
                case 0xD: // Channel Aftertouch
                    evt->MIDI.length = 2;
                    break;
                default:
                    // Unknown status, skip
                    continue;
            }

            // Link events together
            if (eventCount > 0) {
                eventStorage[eventCount - 1].head.next = evt;
            }
            evt->head.next = nullptr;
            eventCount++;
        }
        // System Messages - ignored so far
        offset += messageSize;
    }

    return eventCount > 0 ? &eventStorage[0] : nullptr;
}

} // namespace remidy

#endif
