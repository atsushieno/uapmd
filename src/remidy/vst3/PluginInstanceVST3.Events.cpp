#include "cmidi2.h"
#include "remidy.hpp"
#include "../utils.hpp"

#include "PluginFormatVST3.hpp"

using namespace remidy_vst3;


void remidy::PluginInstanceVST3::VST3UmpInputDispatcher::onNoteOn(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t note,
                                                                       uint8_t attributeType, uint16_t velocity, uint16_t attribute) {
    // use IEventList to add Event (kNoteOnEvent)
    auto& el = owner->processDataInputEvents;

    float tuning = 0;
    if (attributeType == CMIDI2_ATTRIBUTE_TYPE_PITCH7_9)
        tuning = (float) ((uint8_t) (attribute >> 9) + (float) (attribute & 0x1F) / 512.0);

    int32_t noteId = -1; // should be alright, UMP has no concept for that

    Event e{};
    e.busIndex = group;
    e.sampleOffset = static_cast<int32_t>(timestamp());
    e.ppqPosition = trackContext()->ppqPosition();
    e.flags = 0;
    e.type = Event::kNoteOnEvent;
    e.noteOn.channel = channel;
    e.noteOn.pitch = note;
    e.noteOn.tuning = tuning;
    e.noteOn.velocity = (float) (velocity / 65535.0);
    e.noteOn.length = 0;
    e.noteOn.noteId = noteId;
    el.addEvent(e);
}

void remidy::PluginInstanceVST3::VST3UmpInputDispatcher::onNoteOff(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t note,
                                                                        uint8_t attributeType, uint16_t velocity, uint16_t attribute) {
    // use IEventList to add Event (kNoteOffEvent)
    auto& el = owner->processDataInputEvents;
    int32_t noteId = -1; // should be alright, UMP has no concept for that

    Event e{};
    e.busIndex = group;
    e.sampleOffset = static_cast<int32_t>(timestamp());
    e.ppqPosition = trackContext()->ppqPosition();
    e.flags = 0;
    e.type = Event::kNoteOffEvent;
    e.noteOff.channel = channel;
    e.noteOff.pitch = note;
    e.noteOff.velocity = (float) (velocity / 65535.0);
    e.noteOff.noteId = noteId;
    el.addEvent(e);
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
    ParamID id = index;
    double value = (double) data / UINT32_MAX;
    if (owner == nullptr)
        Logger::global()->logInfo("VST3 IMidiMapping does not exist in this plugin. Directly using it as a parameter Id.");
    else if (owner->midi_mapping->getMidiControllerAssignment(group, channel, index, id) != kResultOk)
        Logger::global()->logInfo("VST3 IMidiMapping does not give any mapping to index %d. Directly using it as a parameter Id.", index);
    owner->parameters()->setParameter(index, value, timestamp());
}

void remidy::PluginInstanceVST3::VST3UmpInputDispatcher::onProgramChange(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t flags,
                                                                              remidy::uint7_t program, remidy::uint7_t bankMSB, remidy::uint7_t bankLSB) {
    // we can at best find a VST3 parameter whose channel matches the channel and has kIsProgramChange
    auto parameters = (PluginInstanceVST3::ParameterSupport*) owner->parameters();
    parameters->setProgramChange(group, channel, flags, program, bankMSB, bankLSB);
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
    ParamID id;
    double value = (double) data / UINT32_MAX;
    if (owner->midi_mapping->getMidiControllerAssignment(group, channel, ControllerNumbers::kPitchBend, id) != kResultOk)
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
        ParamID id;
        double value = (double) data / UINT32_MAX;
        if (owner->midi_mapping->getMidiControllerAssignment(group, channel, ControllerNumbers::kAfterTouch, id) != kResultOk)
            Logger::global()->logInfo("VST3 IMidiMapping on this plugin is not working as expected");
        else
            owner->parameters()->setParameter(id, value, timestamp());
    } else {
        auto& el = owner->processDataInputEvents;
        int32_t noteId = -1; // should be alright, UMP has no concept for that

        Event e{};
        e.busIndex = group;
        e.sampleOffset = static_cast<int32_t>(timestamp());
        e.ppqPosition = trackContext()->ppqPosition();
        e.flags = 0;
        e.type = Event::kPolyPressureEvent;
        e.polyPressure.channel = channel;
        e.polyPressure.pitch = perNoteOrMinus;
        e.polyPressure.pressure = static_cast<float>(data / UINT32_MAX);
        e.polyPressure.noteId = noteId;
        el.addEvent(e);
    }
}
