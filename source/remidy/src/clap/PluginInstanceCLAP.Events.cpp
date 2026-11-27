#include <algorithm>
#include <umppi/umppi.hpp>
#include "PluginFormatCLAP.hpp"
#include "../utils.hpp"

namespace remidy {
    void PluginInstanceCLAP::CLAPUmpInputDispatcher::onAC(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t bank, remidy::uint7_t index, uint32_t data, bool relative) {
        // not in CLAP_CORE_EVENT_SPACE_ID
    }
    void PluginInstanceCLAP::CLAPUmpInputDispatcher::onCC(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t index, uint32_t data) {
        // not in CLAP_CORE_EVENT_SPACE_ID
    }
    void PluginInstanceCLAP::CLAPUmpInputDispatcher::onPNAC(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t note, uint8_t index, uint32_t data) {
        // not in CLAP_CORE_EVENT_SPACE_ID
    }
    void PluginInstanceCLAP::CLAPUmpInputDispatcher::onPNRC(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t note, uint8_t index, uint32_t data) {
        // not in CLAP_CORE_EVENT_SPACE_ID
    }
    void PluginInstanceCLAP::CLAPUmpInputDispatcher::addTuningExpression(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t note, double semitones) {
        auto evt = reinterpret_cast<clap_event_note_expression_t *>(owner->events_in->tryAllocate(alignof(void *),
                                                                                       sizeof(clap_event_note_expression_t)));
        if (!evt)
            return;
        evt->header.size = sizeof(clap_event_note_expression_t);
        evt->header.time = timestamp();
        evt->header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        evt->header.type = CLAP_EVENT_NOTE_EXPRESSION;
        evt->header.flags = 0;
        evt->note_id = -1;
        evt->port_index = group;
        evt->channel = channel;
        evt->expression_id = CLAP_NOTE_EXPRESSION_TUNING;
        evt->key = note;
        evt->value = std::clamp(semitones, -120.0, 120.0);
    }

    void PluginInstanceCLAP::CLAPUmpInputDispatcher::onPitchBend(remidy::uint4_t group, remidy::uint4_t channel, int8_t perNoteOrMinus, uint32_t data) {
        if (perNoteOrMinus >= 0) {
            const auto key = static_cast<remidy::uint7_t>(perNoteOrMinus);
            addTuningExpression(group, channel, key,
                                note_tuning_offsets[(channel & 0xF) * 128 + (key & 0x7F)] + perNotePitchBendSemitones(data));
        } else {
            // not in CLAP_CORE_EVENT_SPACE_ID
        }
    }

    void PluginInstanceCLAP::CLAPUmpInputDispatcher::onPressure(remidy::uint4_t group, remidy::uint4_t channel, int8_t perNoteOrMinus, uint32_t data) {
        // FIXME: implement using CLAP_NOTE_EXPRESSION_PRESSURE for PAf., but how about CAf?
    }
    void PluginInstanceCLAP::CLAPUmpInputDispatcher::onProgramChange(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t flags, remidy::uint7_t program, remidy::uint7_t bankMSB, remidy::uint7_t bankLSB) {
        // not in CLAP_CORE_EVENT_SPACE_ID
    }
    void PluginInstanceCLAP::CLAPUmpInputDispatcher::onRC(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t bank, remidy::uint7_t index, uint32_t data, bool relative) {
        // not in CLAP_CORE_EVENT_SPACE_ID
    }
    void PluginInstanceCLAP::CLAPUmpInputDispatcher::onNoteOn(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t note, uint8_t attributeType, uint16_t velocity, uint16_t attribute) {
        auto evt = reinterpret_cast<clap_event_note_t *>(owner->events_in->tryAllocate(alignof(void *),
                                                                                       sizeof(clap_event_note_t)));
        if (!evt)
            return;
        evt->header.size = sizeof(clap_event_note_t);
        evt->header.time = timestamp();
        evt->header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        evt->header.type = CLAP_EVENT_NOTE_ON;
        evt->header.flags = 0;
        evt->note_id = -1;
        evt->port_index = group;
        evt->channel = channel;
        evt->key = note;
        evt->velocity = (double) velocity / UINT16_MAX;

        auto& offset = note_tuning_offsets[(channel & 0xF) * 128 + (note & 0x7F)];
        offset = attributeType == umppi::MidiNoteAttributeType::Pitch7_9 ? pitch7_9OffsetSemitones(note, attribute) : 0.0;
        if (offset != 0.0)
            addTuningExpression(group, channel, note, offset);
    }
    void PluginInstanceCLAP::CLAPUmpInputDispatcher::onNoteOff(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t note, uint8_t attributeType, uint16_t velocity, uint16_t attribute) {
        auto evt = reinterpret_cast<clap_event_note_t *>(owner->events_in->tryAllocate(alignof(void *),
                                                                                       sizeof(clap_event_note_t)));
        if (!evt)
            return;
        evt->header.size = sizeof(clap_event_note_t);
        evt->header.time = timestamp();
        evt->header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        evt->header.type = CLAP_EVENT_NOTE_OFF;
        evt->header.flags = 0;
        evt->note_id = -1;
        evt->port_index = group;
        evt->channel = channel;
        evt->key = note;
        evt->velocity = (double) velocity / UINT16_MAX;
    }

    void PluginInstanceCLAP::CLAPUmpInputDispatcher::onSysEx7(remidy::uint4_t group, const uint8_t* data, size_t size) {
        // tryPush() copies the bytes into the event list heap, which lives until the list is cleared after process().
        clap_event_midi_sysex_t evt{};
        evt.header.size = sizeof(clap_event_midi_sysex_t);
        evt.header.time = timestamp();
        evt.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        evt.header.type = CLAP_EVENT_MIDI_SYSEX;
        evt.header.flags = 0;
        evt.port_index = group;
        evt.buffer = data;
        evt.size = static_cast<uint32_t>(size);
        owner->events_in->tryPush(&evt.header);
    }

}
