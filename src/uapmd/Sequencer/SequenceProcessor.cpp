#include "uapmd/uapmd.hpp"

namespace uapmd {

    class SequenceProcessorImpl : public SequenceProcessor {
        size_t audio_buffer_size_in_frames;
        size_t ump_buffer_size_in_ints;
        uint32_t default_input_channels_{2};
        uint32_t default_output_channels_{2};
        std::vector<std::unique_ptr<AudioPluginTrack>> tracks_{};
        SequenceProcessContext sequence{};
        int32_t sampleRate;
        AudioPluginHostingAPI* pal;

    public:
        explicit SequenceProcessorImpl(int32_t sampleRate, size_t audioBufferSizeInFrames, size_t umpBufferSizeInInts, AudioPluginHostingAPI* pal);

        SequenceProcessContext& data() override { return sequence; }

        std::vector<AudioPluginTrack*>& tracks() const override;

        void addSimpleTrack(std::string& format, std::string& pluginId, uint32_t inputChannels, uint32_t outputChannels, std::function<void(AudioPluginTrack* track, std::string error)> callback) override;
        void setDefaultChannels(uint32_t inputChannels, uint32_t outputChannels) override;
        void addSimplePluginTrack(std::string& format, std::string& pluginId, std::function<void(AudioPluginNode* node, AudioPluginTrack* track, int32_t trackIndex, std::string error)> callback) override;
        void addPluginToTrack(int32_t trackIndex, std::string& format, std::string& pluginId, std::function<void(AudioPluginNode* node, std::string error)> callback) override;
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

    void SequenceProcessorImpl::setDefaultChannels(uint32_t inputChannels, uint32_t outputChannels) {
        default_input_channels_ = inputChannels;
        default_output_channels_ = outputChannels;
    }

    void SequenceProcessorImpl::addSimplePluginTrack(std::string& format, std::string& pluginId, std::function<void(AudioPluginNode* node, AudioPluginTrack* track, int32_t trackIndex, std::string error)> callback) {
        pal->createPluginInstance(sampleRate, default_input_channels_, default_output_channels_, false, format, pluginId, [this,callback](auto node, std::string error) {
            if (!node) {
                callback(nullptr, nullptr, -1, "Could not create simple track: " + error);
                return;
            }

            // Create track and add plugin
            auto* track = addSimpleTrackInternal(std::move(node));
            auto trackIndex = static_cast<int32_t>(tracks_.size() - 1);

            // Configure main bus (moved from AudioPluginSequencer)
            auto trackCtx = sequence.tracks[trackIndex];
            trackCtx->configureMainBus(default_input_channels_, default_output_channels_, audio_buffer_size_in_frames);

            // Get the plugin node
            auto plugins = track->graph().plugins();
            if (plugins.empty()) {
                callback(nullptr, track, trackIndex, "Track has no plugins after instantiation");
                return;
            }

            track->bypassed(false);
            callback(plugins.front(), track, trackIndex, "");
        });
    }

    void SequenceProcessorImpl::addPluginToTrack(int32_t trackIndex, std::string& format, std::string& pluginId, std::function<void(AudioPluginNode* node, std::string error)> callback) {
        // Validate track index
        if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= tracks_.size()) {
            callback(nullptr, std::format("Invalid track index {}", trackIndex));
            return;
        }

        pal->createPluginInstance(sampleRate, default_input_channels_, default_output_channels_, false, format, pluginId, [this, trackIndex, callback](auto node, std::string error) {
            if (!node) {
                callback(nullptr, "Could not create plugin: " + error);
                return;
            }

            // Re-validate track (may have been removed during async operation)
            if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= tracks_.size()) {
                callback(nullptr, std::format("Track {} no longer exists", trackIndex));
                return;
            }

            auto* track = tracks_[static_cast<size_t>(trackIndex)].get();
            auto* nodePtr = node.get();

            // Append to track's graph
            auto status = track->graph().appendNodeSimple(std::move(node));
            if (status != 0) {
                callback(nullptr, std::format("Failed to append plugin to track {} (status {})", trackIndex, status));
                return;
            }

            callback(nodePtr, "");
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
