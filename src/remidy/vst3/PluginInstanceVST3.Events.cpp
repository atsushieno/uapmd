#include <umppi/umppi.hpp>
#include "remidy.hpp"
#include "../utils.hpp"

#include "PluginFormatVST3.hpp"

using namespace remidy_vst3;


void remidy::PluginInstanceVST3::VST3UmpInputDispatcher::onNoteOn(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t note,
                                                                       uint8_t attributeType, uint16_t velocity, uint16_t attribute) {
    // use IEventList to add Event (kNoteOnEvent)
    auto& el = owner->processDataInputEvents;

    float tuning = 0;
    if (attributeType == umppi::MidiNoteAttributeType::Pitch7_9)
        tuning = (float) ((uint8_t) (attribute >> 9) + (float) (attribute & 0x1F) / 512.0);

    int32_t noteId = -1; // should be alright, UMP has no concept for that

    Event e{};
    e.busIndex = group;
    e.sampleOffset = static_cast<int32_t>(timestamp());
    e.ppqPosition = masterContext()->ppqPosition();
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
    e.ppqPosition = masterContext()->ppqPosition();
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
    // Assignable Controller (MIDI 2.0)
    ParamID id = 0;
    double value = (double) data / UINT32_MAX;
    bool mappingFound = false;

    // Check MIDI 2.0 Assignable Controller mappings from IMidiMapping2 - RT-safe
    for (const auto& assignment : owner->cached_midi2_mappings_from_mapping2) {
        if (assignment.busIndex == group &&
            assignment.channel == channel &&
            assignment.controller.registered == false &&
            assignment.controller.bank == bank &&
            assignment.controller.index == index) {
            id = assignment.pId;
            mappingFound = true;
            break;
        }
    }

    if (mappingFound) {
        owner->parameters()->setParameter(id, value, timestamp());
    }
    // If no mapping found, UAPMD handles AC-to-parameter mappings
}

void remidy::PluginInstanceVST3::VST3UmpInputDispatcher::onPNAC(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t note,
                                                                     uint8_t index, uint32_t data) {
    // Per-note controller
    double value = static_cast<double>(data) / UINT32_MAX;
    owner->parameters()->setPerNoteController({ .note = note, .channel = channel, .group = group }, index, value, timestamp());
}

void remidy::PluginInstanceVST3::VST3UmpInputDispatcher::onCC(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t index,
                                                                   uint32_t data) {
    // parameter change, use cached MIDI mappings to resolve index, or directly use it as parameter ID.
    ParamID id = index;
    double value = (double) data / UINT32_MAX;
    bool mappingFound = false;

    // Check MIDI 2.0 mappings from IMidiMapping2 first - RT-safe
    // CC can be mapped as MIDI 2.0 Assignable Controller
    for (const auto& assignment : owner->cached_midi2_mappings_from_mapping2) {
        if (assignment.busIndex == group &&
            assignment.channel == channel &&
            assignment.controller.registered == false &&
            assignment.controller.bank == 0 &&
            assignment.controller.index == index) {
            id = assignment.pId;
            mappingFound = true;
            break;
        }
    }

    // Check MIDI 1.0 mappings from IMidiMapping2 - RT-safe
    if (!mappingFound) {
        for (const auto& assignment : owner->cached_midi1_mappings_from_mapping2) {
            if (assignment.busIndex == group &&
                assignment.channel == channel &&
                assignment.controller == index) {
                id = assignment.pId;
                mappingFound = true;
                break;
            }
        }
    }

    // Fallback to IMidiMapping cache if not found in IMidiMapping2 - RT-safe
    if (!mappingFound) {
        for (const auto& assignment : owner->cached_midi1_mappings_from_mapping) {
            if (assignment.busIndex == group &&
                assignment.channel == channel &&
                assignment.controller == index) {
                id = assignment.pId;
                mappingFound = true;
                break;
            }
        }
    }

    if (!mappingFound) {
        // No cached mapping found - directly use CC index as parameter ID
        Logger::global()->logInfo("VST3 MIDI mapping does not give any mapping to CC index %d. Directly using it as a parameter Id.", index);
    }

    owner->parameters()->setParameter(id, value, timestamp());
}

void remidy::PluginInstanceVST3::VST3UmpInputDispatcher::onProgramChange(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t flags,
                                                                              remidy::uint7_t program, remidy::uint7_t bankMSB, remidy::uint7_t bankLSB) {
    // we can at best find a VST3 parameter whose channel matches the channel and has kIsProgramChange
    auto parameters = (PluginInstanceVST3::ParameterSupport*) owner->parameters();
    parameters->setProgramChange(group, channel, flags, program, bankMSB, bankLSB);
}

void remidy::PluginInstanceVST3::VST3UmpInputDispatcher::onRC(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t bank,
                                                                   remidy::uint7_t index, uint32_t data, bool relative) {
    // Registered Controller (MIDI 2.0)
    // Use cached MIDI 2.0 mappings to resolve RC to parameter ID
    ParamID id = 0;
    double value = (double) data / UINT32_MAX;
    bool mappingFound = false;

    // Use cached MIDI 2.0 mappings from IMidiMapping2 (RT-safe)
    // Note: IMidiMapping doesn't support MIDI 2.0 controllers, so no fallback needed
    for (const auto& assignment : owner->cached_midi2_mappings_from_mapping2) {
        if (assignment.busIndex == group &&
            assignment.channel == channel &&
            assignment.controller.registered == true &&
            assignment.controller.bank == bank &&
            assignment.controller.index == index) {
            id = assignment.pId;
            mappingFound = true;
            break;
        }
    }

    if (mappingFound) {
        owner->parameters()->setParameter(id, value, timestamp());
    }
}

void remidy::PluginInstanceVST3::VST3UmpInputDispatcher::onPNRC(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t note,
                                                                     uint8_t index, uint32_t data) {
    // nothing is mapped here
}

void remidy::PluginInstanceVST3::VST3UmpInputDispatcher::onPitchBend(remidy::uint4_t group, remidy::uint4_t channel, int8_t perNoteOrMinus, uint32_t data) {
    // Note: Pitch bend is a dedicated MIDI message type in both MIDI 1.0 and MIDI 2.0
    // It is represented using ControllerNumbers::kPitchBend in MIDI 1.0 mappings
    ParamID id = 0;
    double value = (double) data / UINT32_MAX;
    bool mappingFound = false;

    // Check MIDI 1.0 mappings from IMidiMapping2 - RT-safe
    for (const auto& assignment : owner->cached_midi1_mappings_from_mapping2) {
        if (assignment.busIndex == group &&
            assignment.channel == channel &&
            assignment.controller == ControllerNumbers::kPitchBend) {
            id = assignment.pId;
            mappingFound = true;
            break;
        }
    }

    // Fallback to IMidiMapping cache - RT-safe
    if (!mappingFound) {
        for (const auto& assignment : owner->cached_midi1_mappings_from_mapping) {
            if (assignment.busIndex == group &&
                assignment.channel == channel &&
                assignment.controller == ControllerNumbers::kPitchBend) {
                id = assignment.pId;
                mappingFound = true;
                break;
            }
        }
    }

    if (!mappingFound) {
        // No cached mapping found - UAPMD handles pitch bend
        return;
    }

    if (perNoteOrMinus < 0)
        owner->parameters()->setParameter(id, value, timestamp());
    else
        owner->parameters()->setPerNoteController(
            { .note = static_cast<uint8_t>(perNoteOrMinus), .channel = channel, .group = group },
            id, value, timestamp());
}

void remidy::PluginInstanceVST3::VST3UmpInputDispatcher::onPressure(remidy::uint4_t group, remidy::uint4_t channel,
                                                                         int8_t perNoteOrMinus, uint32_t data) {
    // CAf: use parameter kAfterTouch (if available)
    // PAf: use IEventList to add Event (kPolyPressureEvent)

    if (perNoteOrMinus < 0) {
        // Channel aftertouch
        // Note: Channel pressure is a dedicated MIDI message type in both MIDI 1.0 and MIDI 2.0
        // It is represented using ControllerNumbers::kAfterTouch in MIDI 1.0 mappings
        // There is no separate MIDI 2.0 controller representation for channel pressure
        ParamID id = 0;
        double value = (double) data / UINT32_MAX;
        bool mappingFound = false;

        // Check MIDI 1.0 mappings from IMidiMapping2 - RT-safe
        for (const auto& assignment : owner->cached_midi1_mappings_from_mapping2) {
            if (assignment.busIndex == group &&
                assignment.channel == channel &&
                assignment.controller == ControllerNumbers::kAfterTouch) {
                id = assignment.pId;
                mappingFound = true;
                break;
            }
        }

        // Fallback to IMidiMapping cache - RT-safe
        if (!mappingFound) {
            for (const auto& assignment : owner->cached_midi1_mappings_from_mapping) {
                if (assignment.busIndex == group &&
                    assignment.channel == channel &&
                    assignment.controller == ControllerNumbers::kAfterTouch) {
                    id = assignment.pId;
                    mappingFound = true;
                    break;
                }
            }
        }

        if (mappingFound) {
            owner->parameters()->setParameter(id, value, timestamp());
        }
        // If no mapping found, channel aftertouch is ignored
    } else {
        // Polyphonic aftertouch - not supported by mapping interfaces
        // Send as VST3 event instead
        auto& el = owner->processDataInputEvents;
        int32_t noteId = -1; // should be alright, UMP has no concept for that

        Event e{};
        e.busIndex = group;
        e.sampleOffset = static_cast<int32_t>(timestamp());
        e.ppqPosition = masterContext()->ppqPosition();
        e.flags = 0;
        e.type = Event::kPolyPressureEvent;
        e.polyPressure.channel = channel;
        e.polyPressure.pitch = perNoteOrMinus;
        e.polyPressure.pressure = static_cast<float>(data / UINT32_MAX);
        e.polyPressure.noteId = noteId;
        el.addEvent(e);
    }
}
