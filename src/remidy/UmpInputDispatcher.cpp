#include "remidy.hpp"
#include "cmidi2.h"

void remidy::TypedUmpInputDispatcher::process(uint64_t newTimestamp, AudioProcessContext &src) {
    _timestamp = newTimestamp;
    track_context = src.trackContext();

    auto ptr = src.midiIn().getMessages();
    auto numBytes = src.midiIn().sizeInBytes();
    CMIDI2_UMP_SEQUENCE_FOREACH(ptr, numBytes, iter) {
        auto ump = (cmidi2_ump *) iter;
        uint7_t group, channel, note;
        bool relative;
        switch (cmidi2_ump_get_message_type(ump)) {
            case CMIDI2_MESSAGE_TYPE_UTILITY:
                switch (cmidi2_ump_get_status_code(ump)) {
                    case CMIDI2_UTILITY_STATUS_DCTPQ:
                        track_context->deltaClockstampTicksPerQuarterNotes(cmidi2_ump_get_dctpq(ump));
                        break;
                    case CMIDI2_UTILITY_STATUS_JR_TIMESTAMP:
                        // FIXME: convert to sample-based timestamp (here?)
                        _timestamp += cmidi2_ump_get_jr_timestamp_timestamp(ump);
                        break;
                    case CMIDI2_UTILITY_STATUS_DELTA_CLOCKSTAMP:
                        // FIXME: implement (calculate timestamp delta)
                        break;
                }
                break;
            case CMIDI2_MESSAGE_TYPE_MIDI_2_CHANNEL:
                group = cmidi2_ump_get_group(ump);
                channel = cmidi2_ump_get_channel(ump);
                note = -1;
                relative = false;
                switch (cmidi2_ump_get_status_code(ump)) {
                    case CMIDI2_STATUS_NOTE_ON:
                        onNoteOn(group, channel,
                                 cmidi2_ump_get_midi2_note_note(ump),
                                 cmidi2_ump_get_midi2_note_attribute_type(ump),
                                 cmidi2_ump_get_midi2_note_velocity(ump),
                                 cmidi2_ump_get_midi2_note_attribute_data(ump));
                        break;
                    case CMIDI2_STATUS_NOTE_OFF:
                        onNoteOff(group, channel,
                                  cmidi2_ump_get_midi2_note_note(ump),
                                  cmidi2_ump_get_midi2_note_attribute_type(ump),
                                  cmidi2_ump_get_midi2_note_velocity(ump),
                                  cmidi2_ump_get_midi2_note_attribute_data(ump));
                        break;
                    case CMIDI2_STATUS_PAF:
                        note = cmidi2_ump_get_midi2_paf_note(ump);
                    case CMIDI2_STATUS_CAF:
                        onPressure(group, channel, note, cmidi2_ump_get_midi2_paf_data(ump));
                        break;
                    case CMIDI2_STATUS_CC:
                        onCC(group, channel,
                             cmidi2_ump_get_midi2_cc_index(ump),
                             cmidi2_ump_get_midi2_cc_data(ump));
                        break;
                    case CMIDI2_STATUS_PROGRAM:
                        onProgramChange(group, channel,
                                        cmidi2_ump_get_midi2_program_options(ump),
                                        cmidi2_ump_get_midi2_program_program(ump),
                                        cmidi2_ump_get_midi2_program_bank_msb(ump),
                                        cmidi2_ump_get_midi2_program_bank_lsb(ump));
                        break;
                    case CMIDI2_STATUS_PER_NOTE_PITCH_BEND:
                        note = cmidi2_ump_get_midi2_pn_pitch_bend_note(ump);
                    case CMIDI2_STATUS_PITCH_BEND:
                        onPitchBend(group, channel, note, cmidi2_ump_get_midi2_pitch_bend_data(ump));
                        break;
                    case CMIDI2_STATUS_PER_NOTE_RCC:
                        onPNRC(group, channel,
                               cmidi2_ump_get_midi2_pnrcc_note(ump),
                               cmidi2_ump_get_midi2_pnrcc_index(ump),
                               cmidi2_ump_get_midi2_pnrcc_data(ump));
                    case CMIDI2_STATUS_PER_NOTE_ACC:
                        onPNAC(group, channel,
                               cmidi2_ump_get_midi2_pnacc_note(ump),
                               cmidi2_ump_get_midi2_pnacc_index(ump),
                               cmidi2_ump_get_midi2_pnacc_data(ump));
                        break;
                    case CMIDI2_STATUS_RELATIVE_RPN:
                        relative = true;
                    case CMIDI2_STATUS_RPN:
                        onRC(group, channel,
                             cmidi2_ump_get_midi2_rpn_msb(ump),
                             cmidi2_ump_get_midi2_rpn_lsb(ump),
                             cmidi2_ump_get_midi2_rpn_data(ump),
                             relative);
                        break;
                    case CMIDI2_STATUS_RELATIVE_NRPN:
                        relative = true;
                    case CMIDI2_STATUS_NRPN:
                        onAC(group, channel,
                             cmidi2_ump_get_midi2_nrpn_msb(ump),
                             cmidi2_ump_get_midi2_nrpn_lsb(ump),
                             cmidi2_ump_get_midi2_nrpn_data(ump),
                             relative);
                        break;
                    case CMIDI2_STATUS_PER_NOTE_MANAGEMENT:
                        onPerNoteManagement(group, channel,
                                // FIXME: we should not need this cast
                                            (uint7_t) cmidi2_ump_get_midi2_pn_management_note(ump),
                                            cmidi2_ump_get_midi2_pn_management_options(ump));
                        break;
                }
                break;
        }
    }
    track_context = nullptr;
}
