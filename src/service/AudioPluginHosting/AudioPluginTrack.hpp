#pragma once
#include "../Common/CommonTypes.hpp"
#include "AudioPluginGraph.hpp"
#include "../Common/MidiSequence.hpp"
#include "../Common/AudioBufferList.hpp"

class AudioPluginTrack {
    class Impl;
    Impl* impl;

public:
    AudioPluginTrack();
    ~AudioPluginTrack();

    AudioPluginGraph& getGraph();

    bool isBypass();
    bool isFrozen();
    void setBypass(bool value);
    void setFrozen(bool value);

    int32_t processAudio(AudioBufferList *audio_buffers, MidiSequence *midi_sequence);
};
