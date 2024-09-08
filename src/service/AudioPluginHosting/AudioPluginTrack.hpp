#pragma once
#include "../Common/CommonTypes.hpp"
#include "../Common/MidiSequence.hpp"
#include "../Common/AudioBufferList.hpp"

class AudioPluginTrack {
public:
    uapmd_status_t sendUmp(MidiSequence* midiInput, AudioBufferList audioOutputs);
};
