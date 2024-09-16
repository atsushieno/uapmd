#pragma once
#include <memory>

#include "AudioPluginTrack.hpp"
#include "../Common/AudioBufferList.hpp"
#include "../Common/MidiSequence.hpp"

class AudioPluginHost {
    class Impl;
    Impl *impl;

public:
    AudioPluginHost();
    ~AudioPluginHost();

    AudioPluginTrack* getTrack(int32_t index);
    uapmd_status_t processAudio(AudioBufferList *audioBufferList, MidiSequence *midiSequence);
};
