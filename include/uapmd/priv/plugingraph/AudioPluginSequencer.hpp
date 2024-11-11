#pragma once
#include <memory>

#include "AudioPluginTrack.hpp"
#include "uapmd/priv/SequenceData.hpp"
#include "remidy/remidy.hpp"

namespace uapmd {
    class AudioPluginHostPAL;

    class AudioPluginSequencer {
        class Impl;
        Impl *impl;

    public:
        explicit AudioPluginSequencer(AudioPluginHostPAL* pal = AudioPluginHostPAL::instance());
        ~AudioPluginSequencer();

        std::vector<AudioPluginTrack *> & tracks() const;

        void addSimpleTrack(uint32_t sampleRate, std::string& format, std::string& pluginId, std::function<void(std::string error)> callback);

        uapmd_status_t processAudio(SequenceData& data);
    };

}
