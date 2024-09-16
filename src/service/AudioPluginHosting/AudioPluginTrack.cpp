
#include "AudioPluginTrack.hpp"

class AudioPluginTrack::Impl {
public:
    bool bypass{false};
    bool frozen{false};
    AudioPluginGraph graph{};
    int32_t processAudio(AudioBufferList *audio_buffers, MidiSequence *midi_sequence);
};

int32_t AudioPluginTrack::Impl::processAudio(AudioBufferList *audio_buffers, MidiSequence *midi_sequence) {
    return graph.processAudio(audio_buffers, midi_sequence);
}

AudioPluginTrack::AudioPluginTrack() {
}

AudioPluginTrack::~AudioPluginTrack() {
}

AudioPluginGraph& AudioPluginTrack::getGraph() {
    return impl->graph;
}

bool AudioPluginTrack::isBypass() { return impl->bypass; }

bool AudioPluginTrack::isFrozen() { return impl->frozen; }

void AudioPluginTrack::setBypass(bool value) { impl->bypass = value; }

void AudioPluginTrack::setFrozen(bool value) { impl->frozen = value; }

int32_t AudioPluginTrack::processAudio(AudioBufferList *audio_buffers, MidiSequence *midi_sequence) {
    return impl->processAudio(audio_buffers, midi_sequence);
}
