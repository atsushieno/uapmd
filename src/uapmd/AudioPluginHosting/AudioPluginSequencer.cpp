#include "uapmd/uapmd.hpp"

namespace uapmd {

    class AudioPluginSequencer::Impl {
        std::vector<AudioPluginTrack*> tracks{};
    public:
        explicit Impl(AudioPluginHostPAL* pal);
        ~Impl();

        AudioPluginHostPAL* pal;

        std::vector<AudioPluginTrack*>& getTracks();

        void addSimpleTrack(AudioPluginNode* node);

        uapmd_status_t processAudio(SequenceData& data);
    };

    AudioPluginSequencer::AudioPluginSequencer(AudioPluginHostPAL* pal) {
        impl = new Impl(pal);
    }

    AudioPluginSequencer::~AudioPluginSequencer() {
        delete impl;
    }

    std::vector<AudioPluginTrack *> &AudioPluginSequencer::tracks() {
        return impl->getTracks();
    }

    uapmd_status_t AudioPluginSequencer::processAudio(SequenceData& data) {
        return impl->processAudio(data);
    }

    void AudioPluginSequencer::addSimpleTrack(std::string &format, std::string &pluginId, std::function<void(std::string error)>&& callback) {
        impl->pal->createPluginInstance(format, pluginId, [&](AudioPluginNode* node, std::string error) {
            if (!node)
                callback("Could not create simple track: " + error);
            else
                impl->addSimpleTrack(node);
        });
    }

    // Impl
    AudioPluginSequencer::Impl::Impl(AudioPluginHostPAL* pal) : pal(pal) {}

    AudioPluginSequencer::Impl::~Impl() {
        for (auto track : tracks)
            delete track;

        delete pal;
    }

    std::vector<AudioPluginTrack*> &AudioPluginSequencer::Impl::getTracks() {
        return tracks;
    }

    int32_t AudioPluginSequencer::Impl::processAudio(SequenceData& data) {
        if (tracks.size() != data.tracks.size())
            // FIXME: define status codes
            return 1;
        for (auto i = 0; i < data.tracks.size(); i++)
            tracks[i]->processAudio(*data.tracks[i]);
        // FIXME: define status codes
        return 0;
    }

    void AudioPluginSequencer::Impl::addSimpleTrack(AudioPluginNode *node) {
        auto track = new AudioPluginTrack();
        track->getGraph().appendNodeSimple(node);
        tracks.emplace_back(track);
    }

}
