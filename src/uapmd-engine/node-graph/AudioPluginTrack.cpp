
#include <functional>
#include <memory>
#include <vector>
#include "uapmd-engine/uapmd-engine.hpp"

namespace uapmd {
    class AudioPluginTrackImpl : public AudioPluginTrack {
        std::function<uint8_t(int32_t)> group_resolver_;
        std::function<void(int32_t, const uapmd_ump_t*, size_t)> event_output_callback_;
        bool bypass_{false};
        bool frozen_{false};
        std::unique_ptr<AudioPluginGraph> graph_;

    public:
        explicit AudioPluginTrackImpl(size_t eventBufferSizeInBytes);

        AudioPluginGraph& graph() override { return *graph_; }

        bool bypassed() override { return bypass_; }
        bool frozen() override { return frozen_; }
        void bypassed(bool value) override { bypass_ = value; }
        void frozen(bool value) override { frozen_ = value; }

        int32_t processAudio(AudioProcessContext& process) override;
        void setGroupResolver(std::function<uint8_t(int32_t)> resolver) override;
        void setEventOutputCallback(std::function<void(int32_t, const uapmd_ump_t*, size_t)> callback) override;
        bool isProcessing() const override { return false; } // No longer used, protection is at SequencerEngine level
    };

    AudioPluginTrackImpl::AudioPluginTrackImpl(size_t eventBufferSizeInBytes) :
        graph_(AudioPluginGraph::create(eventBufferSizeInBytes)) {
    }

    int32_t AudioPluginTrackImpl::processAudio(AudioProcessContext& process) {
        process.clearAudioOutputs();

        auto plugins = graph_->plugins();
        if (plugins.empty())
            return 0;

        return graph_->processAudio(process, group_resolver_, event_output_callback_);
    }

    void AudioPluginTrackImpl::setGroupResolver(std::function<uint8_t(int32_t)> resolver) {
        group_resolver_ = std::move(resolver);
    }

    void AudioPluginTrackImpl::setEventOutputCallback(std::function<void(int32_t, const uapmd_ump_t*, size_t)> callback) {
        event_output_callback_ = std::move(callback);
    }

    std::unique_ptr<AudioPluginTrack> AudioPluginTrack::create(size_t eventBufferSizeInBytes) {
        return std::make_unique<AudioPluginTrackImpl>(eventBufferSizeInBytes);
    }

}
