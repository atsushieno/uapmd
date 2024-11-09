#include "uapmd/uapmd.hpp"

namespace uapmd {

    class AudioPluginSequencer::Impl {
        std::vector<AudioPluginTrack*> tracks{};
    public:
        explicit Impl(AudioPluginHostPAL* pal);
        ~Impl();

        AudioPluginHostPAL* pal;

        std::vector<AudioPluginTrack*>& getTracks();

        void addSimpleTrack(std::unique_ptr<AudioPluginNode> node);

        uapmd_status_t processAudio(SequenceData& data);
    };

    AudioPluginSequencer::AudioPluginSequencer(AudioPluginHostPAL* pal) {
        impl = new Impl(pal);
    }

    AudioPluginSequencer::~AudioPluginSequencer() {
        delete impl;
    }

    std::vector<AudioPluginTrack *> & AudioPluginSequencer::tracks() const {
        return impl->getTracks();
    }

    uapmd_status_t AudioPluginSequencer::processAudio(SequenceData& data) {
        return impl->processAudio(data);
    }

    void AudioPluginSequencer::addSimpleTrack(std::string &format, std::string &pluginId, std::function<void(std::string error)>&& callback) {
        std::function<void(std::string error)> cb = std::move(callback);
        impl->pal->createPluginInstance(format, pluginId, [this,cb](auto node, std::string error) {
            if (!node)
                cb("Could not create simple track: " + error);
            else {
                impl->addSimpleTrack(std::move(node));
                cb("");
            }
        });
    }

    // Impl
    AudioPluginSequencer::Impl::Impl(AudioPluginHostPAL* pal) : pal(pal) {}

    AudioPluginSequencer::Impl::~Impl() {
        for (auto track : tracks)
            delete track;
        pal = nullptr; // do not delete
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

    void AudioPluginSequencer::Impl::addSimpleTrack(std::unique_ptr<AudioPluginNode> node) {
        auto track = new AudioPluginTrack();
        track->graph().appendNodeSimple(std::move(node));
        tracks.emplace_back(track);
    }

}