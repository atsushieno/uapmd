
#include "AudioPluginTrack.hpp"


namespace uapmd {
    class AudioPluginTrack::Impl {
    public:
        bool bypass{false};
        bool frozen{false};
        AudioPluginGraph graph{};
        int32_t processAudio(AudioProcessContext* process);
    };

    int32_t AudioPluginTrack::Impl::processAudio(AudioProcessContext* process) {
        return graph.processAudio(process);
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

    int32_t AudioPluginTrack::processAudio(AudioProcessContext* process) {
        return impl->processAudio(process);
    }

}
