#pragma once
#include <vector>

#include "AudioPluginNode.hpp"
#include "../Common/AudioBufferList.hpp"
#include "../Common/MidiSequence.hpp"

class AudioPluginGraph {
    class Impl;
    Impl* impl{};
public:
    AudioPluginGraph();
    ~AudioPluginGraph();
    int32_t processAudio(AudioBufferList *audio_buffers, MidiSequence *midi_sequence);
};
