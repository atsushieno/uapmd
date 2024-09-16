
#include "AudioPluginGraph.hpp"

#include <cassert>

class AudioPluginGraph::Impl {
    std::vector<AudioPluginNode*> nodes;

public:
    int32_t processAudio(AudioBufferList *audio_buffers, MidiSequence *midi_sequence);
};

int32_t AudioPluginGraph::Impl::processAudio(AudioBufferList *audio_buffers, MidiSequence *midi_sequence) {
    // FIXME: implement
    assert(false);
}

AudioPluginGraph::AudioPluginGraph() {
    impl = new Impl();
}

AudioPluginGraph::~AudioPluginGraph() {
    delete impl;
}

int32_t AudioPluginGraph::processAudio(AudioBufferList *audio_buffers, MidiSequence *midi_sequence) {
    return impl->processAudio(audio_buffers, midi_sequence);
}
