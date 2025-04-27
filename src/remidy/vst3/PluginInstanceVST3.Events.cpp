#include "remidy.hpp"
#include "../utils.hpp"

#include "PluginFormatVST3.hpp"

using namespace remidy_vst3;


void remidy::PluginInstanceVST3::VST3UmpInputDispatcher::onNoteOn(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t note,
                                                                       uint8_t attributeType, uint16_t velocity, uint16_t attribute) {
    // use IEventList to add Event (kNoteOnEvent)
    auto& el = owner->processDataInputEvents;
    int32_t noteId = -1; // should be alright, UMP has no concept for that
    v3_event_note_on noteOn{channel, note, 0, (float) (velocity / 65535.0), 0, noteId};
    v3_event e{group, static_cast<int32_t>(timestamp()), trackContext()->ppqPosition(), 0,
               v3_event_type::V3_EVENT_NOTE_ON, {.note_on = noteOn}};
    el.vtable->event_list.add_event(&el, &e);
}

void remidy::PluginInstanceVST3::VST3UmpInputDispatcher::onNoteOff(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t note,
                                                                        uint8_t attributeType, uint16_t velocity, uint16_t attribute) {
    // use IEventList to add Event (kNoteOffEvent)
    auto& el = owner->processDataInputEvents;
    int32_t noteId = -1; // should be alright, UMP has no concept for that
    v3_event_note_off noteOff{channel, note, (float) (velocity / 65535.0), noteId};
    v3_event e{group, static_cast<int32_t>(timestamp()), trackContext()->ppqPosition(), 0,
               v3_event_type::V3_EVENT_NOTE_OFF, {.note_off = noteOff}};
    el.vtable->event_list.add_event(&el, &e);
}

void remidy::PluginInstanceVST3::VST3UmpInputDispatcher::onAC(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t bank,
                                                                   remidy::uint7_t index, uint32_t data, bool relative) {
    // parameter change (index = bank * 128 + index)
    auto parameterIndex = bank * 0x80 + index;
    double value = (double) data / UINT32_MAX;
    owner->parameters()->setParameter(parameterIndex, value, timestamp());
}

void remidy::PluginInstanceVST3::VST3UmpInputDispatcher::onPNAC(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t note,
                                                                     uint8_t index, uint32_t data) {
    // Per-note controller
    double value = static_cast<double>(data) / UINT32_MAX;
    owner->parameters()->setPerNoteController({ .note = note, .channel = channel, .group = group }, index, value, timestamp());
}

void remidy::PluginInstanceVST3::VST3UmpInputDispatcher::onCC(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t index,
                                                                   uint32_t data) {
    // parameter change, use IMidiMapping to resolve index, or directly use it as parameter ID.
    v3_param_id id;
    double value = (double) data / UINT32_MAX;
    if (owner == nullptr)
        Logger::global()->logInfo("VST3 IMidiMapping does not exist in this plugin. Directly using it as a parameter Id.");
    else if (owner->midi_mapping->vtable->midi_mapping.get_midi_controller_assignment(owner->midi_mapping, group, channel, index, &id) != V3_OK)
        Logger::global()->logInfo("VST3 IMidiMapping does not give any mapping to index %d. Directly using it as a parameter Id.", index);
    else {
        owner->parameters()->setParameter(id, value, timestamp());
        return;
    }
    owner->parameters()->setParameter(index, value, timestamp());
}

void remidy::PluginInstanceVST3::VST3UmpInputDispatcher::onProgramChange(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t flags,
                                                                              remidy::uint7_t program, remidy::uint7_t bankMSB, remidy::uint7_t bankLSB) {
    // use IUnitInfo to set program
    if (!owner->unit_info) {
        Logger::global()->logInfo("This VST3 plugin does not support IUnitInfo interface");
        return; // program list is not supported in this plugin
    }
    int32_t index = (bankMSB << 14) + (bankLSB << 7) + program;
    auto count = owner->unit_info->vtable->unit_info.get_unit_count(owner->unit_info);
    if (index < count)
        owner->unit_info->vtable->unit_info.select_unit(owner->unit_info, index);
    else
        Logger::global()->logInfo("This VST3 plugin does not have unit at index %d (count: %d)", index, count);
}

void remidy::PluginInstanceVST3::VST3UmpInputDispatcher::onRC(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t bank,
                                                                   remidy::uint7_t index, uint32_t data, bool relative) {
    // nothing is mapped here
}

void remidy::PluginInstanceVST3::VST3UmpInputDispatcher::onPNRC(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t note,
                                                                     uint8_t index, uint32_t data) {
    // nothing is mapped here
}

void remidy::PluginInstanceVST3::VST3UmpInputDispatcher::onPitchBend(remidy::uint4_t group, remidy::uint4_t channel, int8_t perNoteOrMinus, uint32_t data) {
    // use parameter kPitchBend (if available)
    if (!owner->midi_mapping)
        return;
    v3_param_id id;
    double value = (double) data / UINT32_MAX;
    if (owner->midi_mapping->vtable->midi_mapping.get_midi_controller_assignment(owner->midi_mapping, group, channel, V3_PITCH_BEND, &id) != V3_OK)
        Logger::global()->logInfo("VST3 IMidiMapping on this plugin is not working as expected");
    if (perNoteOrMinus < 0)
        owner->parameters()->setParameter(id, value, timestamp());
    else
        owner->parameters()->setPerNoteController(
            { .note = static_cast<uint8_t>(perNoteOrMinus), .channel = channel, .group = group },
            id, perNoteOrMinus, timestamp());
}

void remidy::PluginInstanceVST3::VST3UmpInputDispatcher::onPressure(remidy::uint4_t group, remidy::uint4_t channel,
                                                                         int8_t perNoteOrMinus, uint32_t data) {
    // CAf: use parameter kAfterTouch (if available)
    // PAf: use IEventList to add Event (kPolyPressureEvent)
    if (perNoteOrMinus < 0) {
        if (!owner->midi_mapping)
            return;
        v3_param_id id;
        double value = (double) data / UINT32_MAX;
        if (owner->midi_mapping->vtable->midi_mapping.get_midi_controller_assignment(owner->midi_mapping, group, channel, V3_AFTER_TOUCH, &id) != V3_OK)
            Logger::global()->logInfo("VST3 IMidiMapping on this plugin is not working as expected");
        else
            owner->parameters()->setParameter(id, value, timestamp());
    } else {
        auto& el = owner->processDataInputEvents;
        int32_t noteId = -1; // should be alright, UMP has no concept for that
        v3_event_poly_pressure paf{channel, perNoteOrMinus, static_cast<float>(data / UINT32_MAX), noteId};
        v3_event e{group, static_cast<int32_t>(timestamp()), trackContext()->ppqPosition(), 0,
                   v3_event_type::V3_EVENT_POLY_PRESSURE, {.poly_pressure = paf}};
        el.vtable->event_list.add_event(&el, &e);
    }
}
