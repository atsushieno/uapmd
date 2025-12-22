#include "cmidi2.h"
#include "uapmd/uapmd.hpp"

namespace uapmd {
    void UapmdUmpInputMapper::process(uint64_t timestamp, remidy::AudioProcessContext& src) {
        auto& inEvents = src.eventIn();
        auto& outEvents = src.eventOut();
        cmidi2_ump_forge forge;
        cmidi2_ump_forge_init(&forge, static_cast<cmidi2_ump *>(outEvents.getMessages()), outEvents.maxMessagesInBytes());
        CMIDI2_UMP_SEQUENCE_FOREACH(inEvents.getMessages(), inEvents.position(), iter) {
            const auto u = reinterpret_cast<cmidi2_ump *>(iter);
            switch (cmidi2_ump_get_message_type(u)) {
                case CMIDI2_MESSAGE_TYPE_MIDI_2_CHANNEL: {
                    bool relative{false};
                    switch (cmidi2_ump_get_status_code(u)) {
                        // plugin parameters
                        case CMIDI2_STATUS_RELATIVE_NRPN:
                            relative = true;
                            // continue below
                        case CMIDI2_STATUS_NRPN: {
                            // parameter change (index = bank * 128 + index)
                            const auto bank = cmidi2_ump_get_midi2_nrpn_msb(u);
                            const auto index = cmidi2_ump_get_midi2_nrpn_lsb(u);
                            const auto data = cmidi2_ump_get_midi2_nrpn_data(u);
                            const auto parameterIndex = bank * 0x80 + index;
                            const double value = relative ?
                                getParameterValue(parameterIndex) + static_cast<double>(data) / INT32_MAX :
                                static_cast<double>(data) / UINT32_MAX;
                            setParameterValue(parameterIndex, value);
                            continue;
                        }
                        // presets
                        case CMIDI2_STATUS_PROGRAM: {
                            const auto bankMsb = cmidi2_ump_get_midi2_program_bank_msb(u);
                            const auto bankLsb = cmidi2_ump_get_midi2_program_bank_lsb(u);
                            const auto program = cmidi2_ump_get_midi2_program_program(u);
                            const auto bankIndex = bankMsb * 0x80 + bankLsb;
                            loadPreset(bankIndex * 0x80 + program);
                            continue;
                        }
                    }
                    break;
                }
            }
            // FIXME: so far, we do not try to create "stripped" UMP sequences, but we likely have to.
            //cmidi2_ump_forge_add_single_packet(&forge, u);
        }
        outEvents.position(forge.offset / sizeof(uapmd_ump_t));
    }
}