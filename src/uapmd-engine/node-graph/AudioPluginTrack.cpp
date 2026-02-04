
#include <functional>
#include <memory>
#include <vector>
#include "uapmd-engine/uapmd-engine.hpp"

namespace uapmd {
    class AudioPluginTrackImpl : public AudioPluginTrack {
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

        return graph_->processAudio(process);
    }

    std::unique_ptr<AudioPluginTrack> AudioPluginTrack::create(size_t eventBufferSizeInBytes) {
        return std::make_unique<AudioPluginTrackImpl>(eventBufferSizeInBytes);
    }

}
