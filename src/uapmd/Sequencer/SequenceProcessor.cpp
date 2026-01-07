#include "uapmd/uapmd.hpp"

namespace uapmd {

    class SequenceProcessorImpl : public SequenceProcessor {
        size_t audio_buffer_size_in_frames;
        size_t ump_buffer_size_in_ints;
        std::vector<std::unique_ptr<AudioPluginTrack>> tracks_{};
        SequenceProcessContext sequence{};
        int32_t sampleRate;
        AudioPluginHostingAPI* pal;

    public:
        explicit SequenceProcessorImpl(int32_t sampleRate, size_t audioBufferSizeInFrames, size_t umpBufferSizeInInts, AudioPluginHostingAPI* pal);

        SequenceProcessContext& data() override { return sequence; }

        std::vector<AudioPluginTrack*>& tracks() const override;

        void addSimpleTrack(std::string& format, std::string& pluginId, uint32_t inputChannels, uint32_t outputChannels, std::function<void(AudioPluginTrack* track, std::string error)> callback) override;
        bool removePluginInstance(int32_t instanceId) override;

        uapmd_status_t processAudio() override;

    private:
        AudioPluginTrack* addSimpleTrackInternal(std::unique_ptr<AudioPluginNode> node);
        void removeTrack(size_t index);
    };

    std::unique_ptr<SequenceProcessor> SequenceProcessor::create(int32_t sampleRate, size_t audioBufferSizeInFrames, size_t umpBufferSizeInInts, AudioPluginHostingAPI* pal) {
        return std::make_unique<SequenceProcessorImpl>(sampleRate, audioBufferSizeInFrames, umpBufferSizeInInts, pal);
    }

    // SequenceProcessorImpl
    SequenceProcessorImpl::SequenceProcessorImpl(int32_t sampleRate, size_t audioBufferSizeInFrames, size_t umpBufferSizeInInts, AudioPluginHostingAPI* pal) :
        audio_buffer_size_in_frames(audioBufferSizeInFrames),
        sampleRate(sampleRate),
        ump_buffer_size_in_ints(umpBufferSizeInInts),
        pal(pal) {
    }

    std::vector<AudioPluginTrack*> &SequenceProcessorImpl::tracks() const {
        // Note: This requires a mutable cache for const correctness
        // Since we need to return a reference to a vector of raw pointers
        static thread_local std::vector<AudioPluginTrack*> track_ptrs;
        track_ptrs.clear();
        for (const auto& track : tracks_)
            track_ptrs.push_back(track.get());
        return track_ptrs;
    }

    int32_t SequenceProcessorImpl::processAudio() {
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

    void SequenceProcessorImpl::addSimpleTrack(std::string& format, std::string& pluginId, uint32_t inputChannels, uint32_t outputChannels, std::function<void(AudioPluginTrack*, std::string error)> callback) {
        pal->createPluginInstance(sampleRate, inputChannels, outputChannels, false, format, pluginId, [this,callback](auto node, std::string error) {
            if (!node)
                callback(nullptr, "Could not create simple track: " + error);
            else {
                auto track = addSimpleTrackInternal(std::move(node));
                callback(track, "");
                track->bypassed(false);
            }
        });
    }

    AudioPluginTrack* SequenceProcessorImpl::addSimpleTrackInternal(std::unique_ptr<AudioPluginNode> node) {
        auto track = AudioPluginTrack::create(ump_buffer_size_in_ints);
        track->graph().appendNodeSimple(std::move(node));
        auto* track_ptr = track.get();
        tracks_.emplace_back(std::move(track));
        sequence.tracks.emplace_back(new AudioProcessContext(sequence.masterContext(), ump_buffer_size_in_ints));
        return track_ptr;
    }

    bool SequenceProcessorImpl::removePluginInstance(int32_t instanceId) {
        for (size_t i = 0; i < tracks_.size(); ++i) {
            auto& track = tracks_[i];
            if (!track)
                continue;
            if (track->graph().removePluginInstance(instanceId)) {
                if (track->graph().plugins().empty())
                    removeTrack(i);
                return true;
            }
        }
        return false;
    }

    void SequenceProcessorImpl::removeTrack(size_t index) {
        if (index >= tracks_.size())
            return;
        tracks_.erase(tracks_.begin() + static_cast<long>(index));
        if (index < sequence.tracks.size()) {
            delete sequence.tracks[index];
            sequence.tracks.erase(sequence.tracks.begin() + static_cast<long>(index));
        }
    }

}
