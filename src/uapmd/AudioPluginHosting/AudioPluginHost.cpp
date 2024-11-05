#include "uapmd/uapmd.hpp"
#include "uapmd/priv/AudioPluginHost.hpp"


namespace uapmd {

    class AudioPluginHost::Impl {
        std::vector<AudioPluginTrack*> tracks{};
    public:
        Impl();
        ~Impl();

        AudioPluginHostPAL* pal;

        std::vector<AudioPluginTrack*>& getTracks();

        void createPluginInstance(std::string &format, std::string &pluginId, std::function<void(AudioPluginNode* node, std::string error)>&& callback);
        void addSimpleTrack(AudioPluginNode* node);

        uapmd_status_t processAudio(std::vector<remidy::AudioProcessContext*> contexts);
    };

    AudioPluginHost::AudioPluginHost() {
        impl = new Impl();
    }

    AudioPluginHost::~AudioPluginHost() {
        delete impl;
    }

    std::vector<AudioPluginTrack *> &AudioPluginHost::tracks() {
        return impl->getTracks();
    }

    uapmd_status_t AudioPluginHost::processAudio(std::vector<remidy::AudioProcessContext*> contexts) {
        return impl->processAudio(contexts);
    }

    void AudioPluginHost::addSimpleTrack(std::string &format, std::string &pluginId, std::function<void(std::string error)>&& callback) {
        impl->pal->createPluginInstance(format, pluginId, [&](AudioPluginNode* node, std::string error) {
            if (!node)
                callback("Could not create simple track: " + error);
            else
                impl->addSimpleTrack(node);
        });
    }

    // Impl

    AudioPluginHost::Impl::~Impl() {
        for (auto track : tracks)
            delete track;

        delete pal;
    }

    std::vector<AudioPluginTrack*> &AudioPluginHost::Impl::getTracks() {
        return tracks;
    }

    int32_t AudioPluginHost::Impl::processAudio(std::vector<remidy::AudioProcessContext*> contexts) {
        if (tracks.size() != contexts.size())
            // FIXME: define status codes
            return 1;
        for (auto i = 0; i < contexts.size(); i++)
            tracks[i]->processAudio(*contexts[i]);
        // FIXME: define status codes
        return 0;
    }

    void AudioPluginHost::Impl::addSimpleTrack(AudioPluginNode *node) {
        auto track = new AudioPluginTrack();
        track->getGraph().appendNodeSimple(node);
        tracks.emplace_back(track);
    }

    AudioPluginHost::Impl::Impl() {
        pal = AudioPluginHostPAL::instance();
    }

}
