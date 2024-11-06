
#include "uapmd/uapmd.hpp"


namespace uapmd {
    class AudioPluginTrack::Impl {
    public:
        bool bypass{false};
        bool frozen{false};
        AudioPluginGraph graph{};
        int32_t processAudio(AudioProcessContext& process);
    };

    int32_t AudioPluginTrack::Impl::processAudio(AudioProcessContext& process) {
        return graph.processAudio(process);
    }

    AudioPluginTrack::AudioPluginTrack() {
        impl = new Impl();
    }

    AudioPluginTrack::~AudioPluginTrack() {
        delete impl;
    }

    AudioPluginGraph& AudioPluginTrack::graph() {
        return impl->graph;
    }

    bool AudioPluginTrack::bypassed() { return impl->bypass; }

    bool AudioPluginTrack::frozen() { return impl->frozen; }

    void AudioPluginTrack::bypassed(bool value) { impl->bypass = value; }

    void AudioPluginTrack::frozen(bool value) { impl->frozen = value; }

    int32_t AudioPluginTrack::processAudio(AudioProcessContext& process) {
        return impl->processAudio(process);
    }

}
