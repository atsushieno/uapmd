#if __APPLE__

#include "PluginFormatAU.hpp"
#include "cmidi2.h"
#import <CoreMIDI/CoreMIDI.h>

namespace remidy {

// Implementation of MIDIEventConverter - converts UMP to AURenderEvent
AURenderEvent* PluginInstanceAUv3::MIDIEventConverter::convertUMPToRenderEvents(EventSequence& eventIn, AUEventSampleTime eventSampleTime) {
    eventCount = 0;

    if (eventIn.position() == 0)
        return nullptr;

    const uint8_t* umpData = (const uint8_t*)eventIn.getMessages();
    size_t umpByteCount = eventIn.position();

    // Use cmidi2 macro to iterate through UMP packets
    CMIDI2_UMP_SEQUENCE_FOREACH(umpData, umpByteCount, iter) {
        if (eventCount >= MAX_EVENTS)
            break;

        const uint32_t* ump = (const uint32_t*)iter;
        uint32_t word0 = *ump;
        uint8_t messageType = cmidi2_ump_get_message_type(&word0);
        uint8_t group = cmidi2_ump_get_group(&word0);
        uint8_t status = cmidi2_ump_get_status_code(&word0);
        uint8_t channel = cmidi2_ump_get_channel(&word0);

        // Convert MIDI 2.0 channel voice messages to MIDI 1.0 for AURenderEvent
        if (messageType == CMIDI2_MESSAGE_TYPE_MIDI_2_CHANNEL) {
            AURenderEvent* evt = &eventStorage[eventCount];
            evt->head.eventType = AURenderEventMIDI;
            evt->head.eventSampleTime = eventSampleTime;
            evt->MIDI.cable = group;
            evt->MIDI.length = 0;

            switch (status) {
                case CMIDI2_STATUS_NOTE_OFF: {
                    uint8_t note = cmidi2_ump_get_midi2_note_note(ump);
                    uint16_t velocity16 = cmidi2_ump_get_midi2_note_velocity(ump);
                    uint8_t velocity = velocity16 >> 9; // 16-bit to 7-bit

                    evt->MIDI.data[0] = 0x80 | channel;
                    evt->MIDI.data[1] = note;
                    evt->MIDI.data[2] = velocity;
                    evt->MIDI.length = 3;
                    break;
                }

                case CMIDI2_STATUS_NOTE_ON: {
                    uint8_t note = cmidi2_ump_get_midi2_note_note(ump);
                    uint16_t velocity16 = cmidi2_ump_get_midi2_note_velocity(ump);
                    uint8_t velocity = velocity16 >> 9;

                    evt->MIDI.data[0] = 0x90 | channel;
                    evt->MIDI.data[1] = note;
                    evt->MIDI.data[2] = velocity;
                    evt->MIDI.length = 3;
                    break;
                }

                case CMIDI2_STATUS_PAF: {
                    uint8_t note = cmidi2_ump_get_midi2_paf_note(ump);
                    uint32_t pressure32 = cmidi2_ump_get_midi2_paf_data(ump);
                    uint8_t pressure = pressure32 >> 25; // 32-bit to 7-bit

                    evt->MIDI.data[0] = 0xA0 | channel;
                    evt->MIDI.data[1] = note;
                    evt->MIDI.data[2] = pressure;
                    evt->MIDI.length = 3;
                    break;
                }

                case CMIDI2_STATUS_CC: {
                    uint8_t cc = cmidi2_ump_get_midi2_cc_index(ump);
                    uint32_t value32 = cmidi2_ump_get_midi2_cc_data(ump);
                    uint8_t value = value32 >> 25; // 32-bit to 7-bit

                    evt->MIDI.data[0] = 0xB0 | channel;
                    evt->MIDI.data[1] = cc;
                    evt->MIDI.data[2] = value;
                    evt->MIDI.length = 3;
                    break;
                }

                case CMIDI2_STATUS_PROGRAM: {
                    uint8_t program = cmidi2_ump_get_midi2_program_program(ump);

                    evt->MIDI.data[0] = 0xC0 | channel;
                    evt->MIDI.data[1] = program;
                    evt->MIDI.length = 2;
                    break;
                }

                case CMIDI2_STATUS_CAF: {
                    uint32_t pressure32 = cmidi2_ump_get_midi2_caf_data(ump);
                    uint8_t pressure = pressure32 >> 25;

                    evt->MIDI.data[0] = 0xD0 | channel;
                    evt->MIDI.data[1] = pressure;
                    evt->MIDI.length = 2;
                    break;
                }

                case CMIDI2_STATUS_PITCH_BEND: {
                    uint32_t bend32 = cmidi2_ump_get_midi2_pitch_bend_data(ump);
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
        else if (messageType == CMIDI2_MESSAGE_TYPE_MIDI_1_CHANNEL) {
            AURenderEvent* evt = &eventStorage[eventCount];
            evt->head.eventType = AURenderEventMIDI;
            evt->head.eventSampleTime = eventSampleTime;
            evt->MIDI.cable = group;

            // MIDI 1.0 messages are already in the right format - just copy the bytes
            uint8_t statusByte = (status << 4) | channel;
            evt->MIDI.data[0] = statusByte;

            // Extract data bytes from the UMP word
            evt->MIDI.data[1] = (word0 >> 8) & 0x7F;
            evt->MIDI.data[2] = word0 & 0x7F;

            // Determine length based on status
            switch (status) {
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
    }

    return eventCount > 0 ? &eventStorage[0] : nullptr;
}

} // namespace remidy

#endif
