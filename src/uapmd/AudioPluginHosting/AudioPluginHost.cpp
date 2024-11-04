#include "uapmd/uapmd.hpp"

namespace uapmd {

    class AudioPluginHost::Impl {
        std::vector<AudioPluginTrack*> tracks{};
    public:
        AudioPluginTrack* getTrack(int32_t index);

        int32_t processAudio(AudioProcessContext* process);
    };

    AudioPluginHost::AudioPluginHost() {
        impl = new Impl();
    }

    AudioPluginHost::~AudioPluginHost() {
        delete impl;
    }

    AudioPluginTrack * AudioPluginHost::getTrack(int32_t index) {
        return impl->getTrack(index);
    }

    uapmd_status_t AudioPluginHost::processAudio(AudioProcessContext* process) {
        // FIXME: we should actually implement UMP dispatching to each track
        return impl->processAudio(process);
    }

    AudioPluginTrack* AudioPluginHost::Impl::getTrack(int32_t index) {
        return tracks[index];
    }

    int32_t AudioPluginHost::Impl::processAudio(AudioProcessContext* process) {
        throw std::runtime_error("Not implemented");
    }

}
