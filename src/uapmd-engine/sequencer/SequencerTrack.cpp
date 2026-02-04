
#include <functional>
#include <memory>
#include <vector>
#include "uapmd-engine/uapmd-engine.hpp"

namespace uapmd {
    class SequencerTrackImpl : public SequencerTrack {
        bool bypass_{false};
        bool frozen_{false};
        std::unique_ptr<AudioPluginGraph> graph_;
        std::vector<int32_t> instance_ids{};

    public:
        explicit SequencerTrackImpl(size_t eventBufferSizeInBytes);

        AudioPluginGraph& graph() override { return *graph_; }

        std::vector<int32_t>& orderedInstanceIds() override {
            return instance_ids;
        }

        bool bypassed() override { return bypass_; }
        bool frozen() override { return frozen_; }
        void bypassed(bool value) override { bypass_ = value; }
        void frozen(bool value) override { frozen_ = value; }
    };

    SequencerTrackImpl::SequencerTrackImpl(size_t eventBufferSizeInBytes) :
        graph_(AudioPluginGraph::create(eventBufferSizeInBytes)) {
    }

    std::unique_ptr<SequencerTrack> SequencerTrack::create(size_t eventBufferSizeInBytes) {
        return std::make_unique<SequencerTrackImpl>(eventBufferSizeInBytes);
    }

}
