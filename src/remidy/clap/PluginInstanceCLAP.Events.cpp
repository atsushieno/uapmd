#include "PluginFormatCLAP.hpp"

namespace remidy {
    void PluginInstanceCLAP::CLAPUmpInputDispatcher::onAC(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t bank, remidy::uint7_t index, uint32_t data, bool relative) {
        // send as param mod or param value event
        auto paramId = bank * 0x80 + index;
        if (relative) {
            auto evt = reinterpret_cast<clap_event_param_mod_t *>(owner->events.tryAllocate(alignof(void *),
                sizeof(clap_event_param_value_t)));
            evt->header.type = CLAP_EVENT_PARAM_MOD;
            evt->port_index = group;
            evt->channel = channel;
            evt->param_id = paramId;
            evt->amount = (double) (int32_t) data / INT32_MAX;
        } else {
            auto evt = reinterpret_cast<clap_event_param_value_t *>(owner->events.tryAllocate(alignof(void *),
                sizeof(clap_event_param_value_t)));
            evt->header.type = CLAP_EVENT_PARAM_VALUE;
            evt->port_index = group;
            evt->channel = channel;
            evt->param_id = paramId;
            evt->value = (double) data / UINT32_MAX;
        }
    }
    void PluginInstanceCLAP::CLAPUmpInputDispatcher::onCC(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t index, uint32_t data) {
        // FIXME: implement
    }
    void PluginInstanceCLAP::CLAPUmpInputDispatcher::onPNAC(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t note, uint8_t index, uint32_t data) {
        // FIXME: implement
    }
    void PluginInstanceCLAP::CLAPUmpInputDispatcher::onPNRC(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t note, uint8_t index, uint32_t data) {
        // FIXME: implement
    }
    void PluginInstanceCLAP::CLAPUmpInputDispatcher::onPitchBend(remidy::uint4_t group, remidy::uint4_t channel, int8_t perNoteOrMinus, uint32_t data) {
        // FIXME: implement
    }
    void PluginInstanceCLAP::CLAPUmpInputDispatcher::onPressure(remidy::uint4_t group, remidy::uint4_t channel, int8_t perNoteOrMinus, uint32_t data) {
        // FIXME: implement
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
        evt->header.type = CLAP_EVENT_NOTE_ON;
        evt->port_index = group;
        evt->channel = channel;
        evt->key = note;
        evt->velocity = (double) velocity / UINT16_MAX;
    }
    void PluginInstanceCLAP::CLAPUmpInputDispatcher::onNoteOff(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t note, uint8_t attributeType, uint16_t velocity, uint16_t attribute) {
        auto evt = reinterpret_cast<clap_event_note_t *>(owner->events.tryAllocate(alignof(void *),
            sizeof(clap_event_param_value_t)));
        evt->header.type = CLAP_EVENT_NOTE_OFF;
        evt->port_index = group;
        evt->channel = channel;
        evt->key = note;
        evt->velocity = (double) velocity / UINT16_MAX;
    }

}