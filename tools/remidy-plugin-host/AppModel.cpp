
#include "AppModel.hpp"
#include <cmidi2.h>

#define DEFAULT_UMP_BUFFER_SIZE 65536
#define DEFAULT_SAMPLE_RATE 48000
uapmd::AppModel model{DEFAULT_UMP_BUFFER_SIZE, DEFAULT_SAMPLE_RATE};

uapmd::AppModel& uapmd::AppModel::instance() { return model; }

void addMessage64(remidy::MidiSequence& midiIn, int64_t ump) {
    cmidi2_ump_write64(midiIn.getMessages() + midiIn.sizeInInts(), ump);
    midiIn.sizeInInts(midiIn.sizeInInts() + 2);
}

void uapmd::AppModel::sendNoteOn(int32_t instanceId, int32_t note) {
    auto trackIndex = trackIndexForInstanceId(instanceId);
    auto buffers = track_buffers[trackIndex];

    auto ump = cmidi2_ump_midi2_note_on(0, 0, note, 0, 0xF800, 0);
    auto& midiIn = buffers->midiIn();
    addMessage64(midiIn, ump);

    std::cerr << std::format("Native note on {}: {}", instanceId, note) << std::endl;
}

void uapmd::AppModel::sendNoteOff(int32_t instanceId, int32_t note) {
    auto trackIndex = trackIndexForInstanceId(instanceId);
    auto buffers = track_buffers[trackIndex];

    auto ump = cmidi2_ump_midi2_note_off(0, 0, note, 0, 0xF800, 0);
    auto& midiIn = buffers->midiIn();
    addMessage64(midiIn, ump);

    std::cerr << std::format("Native note on {}: {}", instanceId, note) << std::endl;
}
