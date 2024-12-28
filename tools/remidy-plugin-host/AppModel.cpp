
#include "AppModel.hpp"
#include <iostream>
#include <cmidi2.h>

#define DEFAULT_AUDIO_BUFFER_SIZE 1024
#define DEFAULT_UMP_BUFFER_SIZE 65536
#define DEFAULT_SAMPLE_RATE 48000

uapmd::AppModel& uapmd::AppModel::instance() {
    static AppModel model{DEFAULT_AUDIO_BUFFER_SIZE, DEFAULT_UMP_BUFFER_SIZE, DEFAULT_SAMPLE_RATE};
    return model;
}

void addMessage64(remidy::EventSequence& eventIn, int64_t ump) {
    cmidi2_ump_write64((cmidi2_ump*) ((uint8_t*) eventIn.getMessages() + eventIn.position()), ump);
    eventIn.position(eventIn.position() + 8);
}

void uapmd::AppModel::sendNoteOn(int32_t instanceId, int32_t note) {
    auto trackIndex = trackIndexForInstanceId(instanceId);
    if (trackIndex < 0) // no instance yet (or no corresponding instance)
        return;
    auto buffers = sequencer.data().tracks[trackIndex];

    auto ump = cmidi2_ump_midi2_note_on(0, 0, note, 0, 0xF800, 0);
    auto& eventIn = buffers->eventIn();
    addMessage64(eventIn, ump);

    std::cerr << std::format("Native note on {}: {}", instanceId, note) << std::endl;
}

void uapmd::AppModel::sendNoteOff(int32_t instanceId, int32_t note) {
    auto trackIndex = trackIndexForInstanceId(instanceId);
    if (trackIndex < 0) // no instance yet (or no corresponding instance)
        return;
    auto buffers = sequencer.data().tracks[trackIndex];

    auto ump = cmidi2_ump_midi2_note_off(0, 0, note, 0, 0xF800, 0);
    auto& eventIn = buffers->eventIn();
    addMessage64(eventIn, ump);

    std::cerr << std::format("Native note on {}: {}", instanceId, note) << std::endl;
}

uapmd::AppModel::AppModel(size_t audioBufferSizeInFrames, size_t umpBufferSizeInBytes, int32_t sampleRate) :
    buffer_size_in_frames(audioBufferSizeInFrames),
            ump_buffer_size_in_bytes(umpBufferSizeInBytes), sample_rate(sampleRate),
            plugin_host_pal(AudioPluginHostPAL::instance()),
            sequencer(sampleRate, buffer_size_in_frames, umpBufferSizeInBytes, this->plugin_host_pal) {

    auto manager = AudioIODeviceManager::instance();
    AudioIODeviceManager::Configuration config{};
    manager->initialize(config);

    auto logger = remidy::Logger::global();
    AudioIODeviceManager::Configuration audioConfig{ .logger = logger };
    manager->initialize(audioConfig);
    // FIXME: enable MIDI devices
    dispatcher.configure(umpBufferSizeInBytes, manager->open());

    dispatcher.addCallback([&](uapmd::AudioProcessContext& process) {
        auto& data = sequencer.data();

        for (uint32_t t = 0, nTracks = sequencer.tracks().size(); t < nTracks; t++) {
            if (t >= data.tracks.size())
                continue; // buffer not ready
            auto ctx = data.tracks[t];
            ctx->eventOut().position(0); // clean up *out* events here.
            ctx->frameCount(process.frameCount());
            for (uint32_t i = 0; i < process.audioInBusCount(); i++) {
                for (uint32_t ch = 0, nCh = process.inputChannelCount(i); ch < nCh; ch++)
                    memcpy(ctx->getFloatInBuffer(i, ch),
                           (void *) process.getFloatInBuffer(i, ch), process.frameCount() * sizeof(float));
            }
        }
        auto ret = sequencer.processAudio();

        for (uint32_t t = 0, nTracks = sequencer.tracks().size(); t < nTracks; t++) {
            if (t >= data.tracks.size())
                continue; // buffer not ready
            auto ctx = data.tracks[t];
            ctx->eventIn().position(0); // clean up *in* events here.
            for (uint32_t i = 0; i < process.audioOutBusCount(); i++) {
                for (uint32_t ch = 0, nCh = ctx->outputChannelCount(i); ch < nCh; ch++)
                    memcpy(process.getFloatOutBuffer(i, ch), (void*) ctx->getFloatOutBuffer(i, ch), process.frameCount() * sizeof(float));
            }
        }
        return ret;
    });
}
