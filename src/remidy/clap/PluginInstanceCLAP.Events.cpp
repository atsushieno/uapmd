#include "PluginFormatCLAP.hpp"

namespace remidy {
    void PluginInstanceCLAP::CLAPUmpInputDispatcher::onAC(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t bank, remidy::uint7_t index, uint32_t data, bool relative) {
        // FIXME: implement
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
        // FIXME: implement
    }
    void PluginInstanceCLAP::CLAPUmpInputDispatcher::onNoteOff(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t note, uint8_t attributeType, uint16_t velocity, uint16_t attribute) {
        // FIXME: implement
    }

}