#include "PluginFormatCLAP.hpp"

namespace remidy {
    void PluginInstanceCLAP::CLAPUmpInputDispatcher::onAC(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t bank, remidy::uint7_t index, uint32_t data, bool relative) {
        auto parameters = owner->parameters();
        // send as param mod or param value event
        auto clapIndex = bank * 0x80 + index;
        if (relative) {
            double current;
            if (parameters->getParameter(index, &current) != StatusCode::OK)
                return;
            auto value = (double) (data + current) / UINT32_MAX;
            parameters->setParameter(clapIndex, value, timestamp());
        } else
            parameters->setParameter(clapIndex, ((double) data) / UINT32_MAX, timestamp());
    }
    void PluginInstanceCLAP::CLAPUmpInputDispatcher::onCC(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t index, uint32_t data) {
        // FIXME: implement
    }
    void PluginInstanceCLAP::CLAPUmpInputDispatcher::onPNAC(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t note, uint8_t index, uint32_t data) {
        PerNoteControllerContext context { .note = note, .channel = channel, .group = group, .extra = 0 };
        owner->parameters()->setPerNoteController(context, index, (double) data / UINT32_MAX, timestamp());
    }
    void PluginInstanceCLAP::CLAPUmpInputDispatcher::onPNRC(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t note, uint8_t index, uint32_t data) {
        // FIXME: implement
    }
    void PluginInstanceCLAP::CLAPUmpInputDispatcher::onPitchBend(remidy::uint4_t group, remidy::uint4_t channel, int8_t perNoteOrMinus, uint32_t data) {
        // FIXME: implement using CLAP_NOTE_EXPRESSION_TUNING https://github.com/free-audio/clap/discussions/203
    }

    void PluginInstanceCLAP::CLAPUmpInputDispatcher::onPressure(remidy::uint4_t group, remidy::uint4_t channel, int8_t perNoteOrMinus, uint32_t data) {
        // FIXME: implement using CLAP_NOTE_EXPRESSION_PRESSURE for PAf., but how about CAf?
    }
    void PluginInstanceCLAP::CLAPUmpInputDispatcher::onProgramChange(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t flags, remidy::uint7_t program, remidy::uint7_t bankMSB, remidy::uint7_t bankLSB) {
        // FIXME: implement
    }
    void PluginInstanceCLAP::CLAPUmpInputDispatcher::onRC(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t bank, remidy::uint7_t index, uint32_t data, bool relative) {
        // FIXME: implement
    }
    void PluginInstanceCLAP::CLAPUmpInputDispatcher::onNoteOn(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t note, uint8_t attributeType, uint16_t velocity, uint16_t attribute) {
        auto evt = reinterpret_cast<clap_event_note_t *>(owner->events.tryAllocate(alignof(void *),
            sizeof(clap_event_param_value_t)));
        if (!evt)
            return;
        evt->header.type = CLAP_EVENT_NOTE_ON;
        evt->port_index = group;
        evt->channel = channel;
        evt->key = note;
        evt->velocity = (double) velocity / UINT16_MAX;
    }
    void PluginInstanceCLAP::CLAPUmpInputDispatcher::onNoteOff(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t note, uint8_t attributeType, uint16_t velocity, uint16_t attribute) {
        auto evt = reinterpret_cast<clap_event_note_t *>(owner->events.tryAllocate(alignof(void *),
            sizeof(clap_event_param_value_t)));
        if (!evt)
            return;
        evt->header.type = CLAP_EVENT_NOTE_OFF;
        evt->port_index = group;
        evt->channel = channel;
        evt->key = note;
        evt->velocity = (double) velocity / UINT16_MAX;
    }

}
