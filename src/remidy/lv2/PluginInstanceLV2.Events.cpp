#include "PluginFormatLV2.hpp"
#include "cmidi2.h"

void remidy::PluginInstanceLV2::LV2UmpInputDispatcher::enqueueMidi1Event(uint8_t atomInIndex, size_t eventSize) {
    int32_t lv2PortIndex = owner->portIndexForAtomGroupIndex(false, atomInIndex);
    auto& forge = owner->lv2_ports[lv2PortIndex].forge;

    auto timestampInSamples = static_cast<int64_t>(timestamp() * 31250 / owner->sample_rate);

    lv2_atom_forge_frame_time(&forge, timestampInSamples);
    lv2_atom_forge_atom(&forge, eventSize, owner->implContext.statics->urids.urid_midi_event_type);
    lv2_atom_forge_write(&forge, midi1Bytes, eventSize);
}

void remidy::PluginInstanceLV2::LV2UmpInputDispatcher::onNoteOn(
        remidy::uint4_t group, remidy::uint4_t channel,
        remidy::uint7_t note, uint8_t attributeType, uint16_t velocity, uint16_t attribute) {
    // It has to downconvert to MIDI 1.0 note on. Group and attribute fields are ignored.
    midi1Bytes[0] = CMIDI2_STATUS_NOTE_ON + channel;
    midi1Bytes[1] = note;
    midi1Bytes[2] = (uint7_t) (velocity >> 9);
    enqueueMidi1Event(group, 3);
}

void remidy::PluginInstanceLV2::LV2UmpInputDispatcher::onNoteOff(
        remidy::uint4_t group, remidy::uint4_t channel,
        remidy::uint7_t note, uint8_t attributeType, uint16_t velocity, uint16_t attribute) {
    // It has to downconvert to MIDI 1.0 note on. Group and attribute fields are ignored.
    // It has to downconvert to MIDI 1.0 note on. Group and attribute fields are ignored.
    midi1Bytes[0] = CMIDI2_STATUS_NOTE_OFF + channel;
    midi1Bytes[1] = note;
    midi1Bytes[2] = (uint7_t) (velocity >> 9);
    enqueueMidi1Event(group, 3);
}

void remidy::PluginInstanceLV2::LV2UmpInputDispatcher::onAC(
        remidy::uint4_t group, remidy::uint4_t channel,
        remidy::uint7_t bank, remidy::uint7_t index, uint32_t data, bool relative) {
    // parameter change (index = bank * 128 + index)
    // FIXME: implement
    // We have to determine whether they run through Atom port or mapped to parameters.
    // If mapped, it has to go through parameter support to dispatch to appropriate port (controlPort or AtomPort)
}

void remidy::PluginInstanceLV2::LV2UmpInputDispatcher::onPNAC(
        remidy::uint4_t group, remidy::uint4_t channel,
        remidy::uint7_t note, uint8_t index, uint32_t data) {
    // LV2 does not support MIDI 2.0, so it cannot be supported
}

void remidy::PluginInstanceLV2::LV2UmpInputDispatcher::onCC(
        remidy::uint4_t group, remidy::uint4_t channel,
        remidy::uint7_t index, uint32_t data) {
    // FIXME: implement
    // We have to determine whether they run through Atom port or mapped to parameters.
    // If mapped, it has to go through parameter support to dispatch to appropriate port (controlPort or AtomPort)
}

void remidy::PluginInstanceLV2::LV2UmpInputDispatcher::onProgramChange(
        remidy::uint4_t group, remidy::uint4_t channel,
        remidy::uint7_t flags, remidy::uint7_t program, remidy::uint7_t bankMSB, remidy::uint7_t bankLSB) {
    if (flags & CMIDI2_PROGRAM_CHANGE_OPTION_BANK_VALID) {
        midi1Bytes[0] = CMIDI2_STATUS_CC + channel;
        midi1Bytes[1] = CMIDI2_CC_BANK_SELECT;
        midi1Bytes[2] = bankMSB;
        midi1Bytes[3] = CMIDI2_STATUS_CC + channel;
        midi1Bytes[4] = CMIDI2_CC_BANK_SELECT_LSB;
        midi1Bytes[5] = bankLSB;
        midi1Bytes[6] = CMIDI2_STATUS_PROGRAM + channel;
        midi1Bytes[7] = program;
        enqueueMidi1Event(group, 8);
    } else {
        midi1Bytes[0] = CMIDI2_STATUS_PROGRAM + channel;
        midi1Bytes[1] = program;
        enqueueMidi1Event(group, 2);
    }
}

void remidy::PluginInstanceLV2::LV2UmpInputDispatcher::onRC(
        remidy::uint4_t group, remidy::uint4_t channel,
        remidy::uint7_t bank, remidy::uint7_t index, uint32_t data, bool relative) {
    midi1Bytes[0] = CMIDI2_STATUS_CC + channel;
    midi1Bytes[1] = index;
    midi1Bytes[2] = (uint7_t) (data >> 9);
    enqueueMidi1Event(group, 3);
}

void remidy::PluginInstanceLV2::LV2UmpInputDispatcher::onPNRC(
        remidy::uint4_t group, remidy::uint4_t channel,
        remidy::uint7_t note, uint8_t index, uint32_t data) {
    // LV2 does not support MIDI 2.0, so it cannot be supported
}

void remidy::PluginInstanceLV2::LV2UmpInputDispatcher::onPitchBend(
        remidy::uint4_t group, remidy::uint4_t channel,
        int8_t perNoteOrMinus, uint32_t data) {
    midi1Bytes[0] = CMIDI2_STATUS_PITCH_BEND + channel;
    midi1Bytes[1] = (uint7_t) (data >> 25);
    midi1Bytes[2] = (uint7_t) ((data >> 17) & 0x7F);
    enqueueMidi1Event(group, 3);
}

void remidy::PluginInstanceLV2::LV2UmpInputDispatcher::onPressure(
        remidy::uint4_t group, remidy::uint4_t channel,
        int8_t perNoteOrMinus, uint32_t data) {
    if (perNoteOrMinus < 0) {
        midi1Bytes[0] = CMIDI2_STATUS_CAF + channel;
        midi1Bytes[1] = (uint7_t) (data >> 25);
        enqueueMidi1Event(group, 2);
    } else {
        midi1Bytes[0] = CMIDI2_STATUS_PAF + channel;
        midi1Bytes[1] = (uint7_t) perNoteOrMinus;
        midi1Bytes[2] = (uint7_t) (data >> 25);
        enqueueMidi1Event(group, 3);
    }
    // CAf and PAf
    // FIXME: implement
}

void remidy::PluginInstanceLV2::LV2UmpInputDispatcher::onProcessStart(remidy::AudioProcessContext &src) {
    for (size_t i = 0, n = owner->lv2_ports.size(); i < n; i++) {
        auto& port = owner->lv2_ports[i];
        lv2_atom_forge_sequence_head(&port.forge, &port.frame, owner->implContext.statics->urids.urid_time_frame);
    }
}

void remidy::PluginInstanceLV2::LV2UmpInputDispatcher::onProcessEnd(remidy::AudioProcessContext &src) {
    for (size_t i = 0, n = owner->lv2_ports.size(); i < n; i++) {
        auto& port = owner->lv2_ports[i];
        lv2_atom_forge_pop(&port.forge, &port.frame);
    }
}
