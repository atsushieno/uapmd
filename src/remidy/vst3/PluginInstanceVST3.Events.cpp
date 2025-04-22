#include "remidy.hpp"
#include "../utils.hpp"

#include "PluginFormatVST3.hpp"

using namespace remidy_vst3;


void remidy::AudioPluginInstanceVST3::VST3UmpInputDispatcher::onNoteOn(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t note,
                                                                       uint8_t attributeType, uint16_t velocity, uint16_t attribute) {
    // use IEventList to add Event (kNoteOnEvent)
    auto& el = owner->processDataInputEvents;
    int32_t noteId = -1; // FIXME: create one
    v3_event_note_on noteOn{channel, note, 0, (float) (velocity / 65535.0), 0, noteId};
    v3_event e{group, static_cast<int32_t>(timestamp()), trackContext()->ppqPosition(), 0,
               v3_event_type::V3_EVENT_NOTE_ON, {.note_on = noteOn}};
    el.vtable->event_list.add_event(&el, &e);
}

void remidy::AudioPluginInstanceVST3::VST3UmpInputDispatcher::onNoteOff(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t note,
                                                                        uint8_t attributeType, uint16_t velocity, uint16_t attribute) {
    // use IEventList to add Event (kNoteOffEvent)
    auto& el = owner->processDataInputEvents;
    int32_t noteId = -1; // FIXME: create one
    v3_event_note_off noteOff{channel, note, (float) (velocity / 65535.0), noteId};
    v3_event e{group, static_cast<int32_t>(timestamp()), trackContext()->ppqPosition(), 0,
               v3_event_type::V3_EVENT_NOTE_ON, {.note_off = noteOff}};
    el.vtable->event_list.add_event(&el, &e);
}

void remidy::AudioPluginInstanceVST3::VST3UmpInputDispatcher::onAC(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t bank,
                                                                   remidy::uint7_t index, uint32_t data, bool relative) {
    // parameter change (index = bank * 128 + index)
    Logger::global()->logInfo("VST3 onAC() is not implemented");
}

void remidy::AudioPluginInstanceVST3::VST3UmpInputDispatcher::onPNAC(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t note,
                                                                     uint8_t index, uint32_t data) {
    // Per-note controller
    Logger::global()->logInfo("VST3 onPNAC() is not implemented");
}

void remidy::AudioPluginInstanceVST3::VST3UmpInputDispatcher::onCC(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t index,
                                                                   uint32_t data) {
    // parameter change, use IMidiMapping to resolve index
    Logger::global()->logInfo("VST3 onCC() is not implemented");
}

void remidy::AudioPluginInstanceVST3::VST3UmpInputDispatcher::onProgramChange(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t flags,
                                                                              remidy::uint7_t program, remidy::uint7_t bankMSB, remidy::uint7_t bankLSB) {
    // use IProgramListData to set program
    Logger::global()->logInfo("VST3 onProgramChange() is not implemented");
}

void remidy::AudioPluginInstanceVST3::VST3UmpInputDispatcher::onRC(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t bank,
                                                                   remidy::uint7_t index, uint32_t data, bool relative) {
    // nothing is mapped here
}

void remidy::AudioPluginInstanceVST3::VST3UmpInputDispatcher::onPNRC(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t note,
                                                                     uint8_t index, uint32_t data) {
    // nothing is mapped here
}

void remidy::AudioPluginInstanceVST3::VST3UmpInputDispatcher::onPitchBend(remidy::uint4_t group, remidy::uint4_t channel, int8_t perNoteOrMinus, uint32_t data) {
    // use parameter kPitchBend (if available)
    Logger::global()->logInfo("VST3 onPitchBend() is not implemented");
}

void remidy::AudioPluginInstanceVST3::VST3UmpInputDispatcher::onPressure(remidy::uint4_t group, remidy::uint4_t channel,
                                                                         int8_t perNoteOrMinus, uint32_t data) {
    // CAf: use parameter kAfterTouch (if available)
    // PAf: use IEventList to add Event (kPolyPressureEvent)
    Logger::global()->logInfo("VST3 onPressure() is not implemented");
}
