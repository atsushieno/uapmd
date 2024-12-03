#include "uapmd/uapmd.hpp"

namespace uapmd {

    class AudioPluginSequencer::Impl {
        remidy::MasterContext master_context{};
        std::vector<AudioPluginTrack*> tracks{};
    public:
        explicit Impl(AudioPluginHostPAL* pal);
        ~Impl();

        AudioPluginHostPAL* pal;

        remidy::MasterContext& masterContext() { return master_context; }

        std::vector<AudioPluginTrack*>& getTracks();

        AudioPluginTrack* addSimpleTrack(std::unique_ptr<AudioPluginNode> node);

        uapmd_status_t processAudio(SequenceData& data);
    };

    AudioPluginSequencer::AudioPluginSequencer(AudioPluginHostPAL* pal) {
        impl = new Impl(pal);
    }

    AudioPluginSequencer::~AudioPluginSequencer() {
        delete impl;
    }

    remidy::MasterContext &AudioPluginSequencer::masterContext() {
        return impl->masterContext();
    }

    std::vector<AudioPluginTrack *> & AudioPluginSequencer::tracks() const {
        return impl->getTracks();
    }

    uapmd_status_t AudioPluginSequencer::processAudio(SequenceData& data) {
        return impl->processAudio(data);
    }

    void AudioPluginSequencer::addSimpleTrack(uint32_t sampleRate, std::string &format, std::string &pluginId, std::function<void(AudioPluginTrack*, std::string error)> callback) {
        impl->pal->createPluginInstance(sampleRate, format, pluginId, [this,callback](auto node, std::string error) {
            if (!node)
                callback(nullptr, "Could not create simple track: " + error);
            else {
                auto track = impl->addSimpleTrack(std::move(node));
                callback(track, "");
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

    AudioPluginTrack* AudioPluginSequencer::Impl::addSimpleTrack(std::unique_ptr<AudioPluginNode> node) {
        auto track = new AudioPluginTrack();
        track->graph().appendNodeSimple(std::move(node));
        tracks.emplace_back(track);
        return track;
    }

}
