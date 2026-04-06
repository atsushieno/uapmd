
#include <algorithm>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>
#include "uapmd-engine/uapmd-engine.hpp"

namespace uapmd {
    class SequencerTrackImpl : public SequencerTrack {
        bool bypass_{false};
        bool frozen_{false};
        std::unique_ptr<AudioPluginGraph> graph_;
        std::vector<int32_t> instance_ids{};
        std::unordered_map<int32_t, uint8_t> instance_groups_{}; // instanceId → UMP group

    public:
        explicit SequencerTrackImpl(std::unique_ptr<AudioPluginGraph>&& graph);
        ~SequencerTrackImpl() override = default;

        AudioPluginGraph& graph() override { return *graph_; }
        bool replaceGraph(std::unique_ptr<AudioPluginGraph>&& graph) override;
        uint32_t latencyInSamples() override { return graph_ ? graph_->mainOutputLatencyInSamples() : 0; }
        uint32_t renderLeadInSamples() override { return graph_ ? graph_->renderLeadInSamples() : 0; }
        double tailLengthInSeconds() override { return graph_ ? graph_->mainOutputTailLengthInSeconds() : 0.0; }

        std::vector<int32_t>& orderedInstanceIds() override {
            return instance_ids;
        }

        bool bypassed() override { return bypass_; }
        bool frozen() override { return frozen_; }
        void bypassed(bool value) override { bypass_ = value; }
        void frozen(bool value) override { frozen_ = value; }

        void setInstanceGroup(int32_t instanceId, uint8_t group) override {
            instance_groups_[instanceId] = group;
        }

        uint8_t getInstanceGroup(int32_t instanceId) const override {
            auto it = instance_groups_.find(instanceId);
            return it != instance_groups_.end() ? it->second : 0xFFu;
        }

        uint8_t findAvailableGroup() const override {
            for (uint8_t g = 0; g < 16; ++g) {
                bool inUse = false;
                for (const auto& [id, grp] : instance_groups_)
                    if (grp == g) { inUse = true; break; }
                if (!inUse) return g;
            }
            return 0xFFu; // all 16 groups taken
        }

        void removeInstance(int32_t instanceId) override {
            std::erase(instance_ids, instanceId);
            instance_groups_.erase(instanceId);
        }
    };

    SequencerTrackImpl::SequencerTrackImpl(std::unique_ptr<AudioPluginGraph>&& graph) :
        graph_(std::move(graph)) {
    }

    std::unique_ptr<SequencerTrack> SequencerTrack::create(
        const AudioGraphProviderRegistry& registry,
        size_t eventBufferSizeInBytes,
        const std::string& graphProviderId) {
        auto graph = registry.createGraph(graphProviderId, eventBufferSizeInBytes);
        if (!graph)
            graph = AudioPluginGraph::create(eventBufferSizeInBytes);
        return std::make_unique<SequencerTrackImpl>(std::move(graph));
    }

    bool SequencerTrackImpl::replaceGraph(std::unique_ptr<AudioPluginGraph>&& graph) {
        if (!graph_ || !graph)
            return false;

        if (!AudioPluginGraph::migrate(*graph, *graph_))
            return false;

        graph_ = std::move(graph);
        return true;
    }

}
