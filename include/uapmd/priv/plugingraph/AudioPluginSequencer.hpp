#pragma once
#include <memory>

#include "AudioPluginTrack.hpp"
#include "uapmd/priv/SequenceData.hpp"
#include "remidy/remidy.hpp"

namespace uapmd {
    class AudioPluginHostPAL;

    // A sequencer for audio plugin tracks.
    // It is independent of DeviceIODispatcher.
    class AudioPluginSequencer {
        class Impl;
        Impl *impl;

    public:
        explicit AudioPluginSequencer(int32_t sampleRate, AudioPluginHostPAL* pal = AudioPluginHostPAL::instance());
        ~AudioPluginSequencer();

        remidy::MasterContext& masterContext();

        std::vector<AudioPluginTrack *> & tracks() const;

        void addSimpleTrack(std::string& format, std::string& pluginId, std::function<void(AudioPluginTrack* track, std::string error)> callback);

        uapmd_status_t processAudio(SequenceData& data);
    };

}
