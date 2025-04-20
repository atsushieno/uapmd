#include "uapmd/uapmd.hpp"

namespace uapmd {

    class SequenceProcessor::Impl {
        size_t audio_buffer_size_in_frames;
        size_t ump_buffer_size_in_ints;
        MasterContext master_context{};
        std::vector<AudioPluginTrack*> tracks_{};
        SequenceProcessContext sequence{};
    public:
        explicit Impl(int32_t sampleRate, size_t audioBufferSizeInFrames, size_t umpBufferSizeInInts, AudioPluginHostPAL* pal);
        ~Impl();

        int32_t sampleRate;

        AudioPluginHostPAL* pal;

        SequenceProcessContext& data() { return sequence; }

        std::vector<AudioPluginTrack*>& tracks();

        AudioPluginTrack* addSimpleTrack(std::unique_ptr<AudioPluginNode> node);

        uapmd_status_t processAudio();
    };

    SequenceProcessor::SequenceProcessor(int32_t sampleRate, size_t audioBufferSizeInFrames, size_t umpBufferSizeInInts, AudioPluginHostPAL* pal) {
        impl = new Impl(sampleRate, audioBufferSizeInFrames, umpBufferSizeInInts, pal);
    }

    SequenceProcessor::~SequenceProcessor() {
        delete impl;
    }

    SequenceProcessContext& SequenceProcessor::data() {
        return impl->data();
    }

    std::vector<AudioPluginTrack *> & SequenceProcessor::tracks() const {
        return impl->tracks();
    }

    uapmd_status_t SequenceProcessor::processAudio() {
        return impl->processAudio();
    }

    void SequenceProcessor::addSimpleTrack(std::string &format, std::string &pluginId, std::function<void(AudioPluginTrack*, std::string error)> callback) {
        impl->pal->createPluginInstance(impl->sampleRate, format, pluginId, [this,callback](auto node, std::string error) {
            if (!node)
                callback(nullptr, "Could not create simple track: " + error);
            else {
                auto track = impl->addSimpleTrack(std::move(node));
                callback(track, "");
            }
        });
    }

    // Impl
    SequenceProcessor::Impl::Impl(int32_t sampleRate, size_t audioBufferSizeInFrames, size_t umpBufferSizeInInts, AudioPluginHostPAL* pal) :
        sampleRate(sampleRate), ump_buffer_size_in_ints(umpBufferSizeInInts), pal(pal) {
    }

    SequenceProcessor::Impl::~Impl() {
        for (auto track : tracks_)
            delete track;
        pal = nullptr; // do not delete
    }

    std::vector<AudioPluginTrack*> &SequenceProcessor::Impl::tracks() {
        return tracks_;
    }

    int32_t SequenceProcessor::Impl::processAudio() {
        if (tracks_.size() != sequence.tracks.size())
            // FIXME: define status codes
            return 1;
        for (auto i = 0; i < sequence.tracks.size(); i++) {
            auto& tp = *sequence.tracks[i];
            tracks_[i]->processAudio(tp);
            tp.eventIn().position(0); // reset
        }
        // FIXME: define status codes
        return 0;
    }

    AudioPluginTrack* SequenceProcessor::Impl::addSimpleTrack(std::unique_ptr<AudioPluginNode> node) {
        auto track = new AudioPluginTrack(ump_buffer_size_in_ints);
        track->graph().appendNodeSimple(std::move(node));
        tracks_.emplace_back(track);
        sequence.tracks.emplace_back(new AudioProcessContext(master_context, ump_buffer_size_in_ints));
        return track;
    }

}
