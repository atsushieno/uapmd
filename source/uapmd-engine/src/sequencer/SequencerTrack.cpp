
#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>
#include <cstring>
#include "uapmd-engine/uapmd-engine.hpp"

using namespace uapmd_graph;

namespace uapmd {
    SequencerTrack::SequencerTrack(size_t eventBufferSizeInBytes)
        : plugin_output_events_(eventBufferSizeInBytes / sizeof(uapmd_ump_t)) {
        static_assert(std::atomic<uint32_t>::is_always_lock_free);
    }

    void SequencerTrack::capturePluginOutput(int32_t instanceId, const uapmd_ump_t* data, size_t bytes) {
        if (!data)
            return;
        const auto* source = reinterpret_cast<const uint8_t*>(data);
        size_t offset = 0;
        while (bytes - offset >= sizeof(uapmd_ump_t)) {
            uapmd_ump_t firstWord;
            std::memcpy(&firstWord, source + offset, sizeof(firstWord));
            const auto messageBytes = static_cast<size_t>(umppi::umpSizeInInts(firstWord >> 28)) * sizeof(uapmd_ump_t);
            if (messageBytes == 0 || messageBytes > 4 * sizeof(uapmd_ump_t) || messageBytes > bytes - offset)
                break;
            if (plugin_output_event_count_ < plugin_output_events_.size()) {
                auto& event = plugin_output_events_[plugin_output_event_count_++];
                event.preset_request = false;
                event.instance_id = instanceId;
                event.size_in_bytes = static_cast<uint32_t>(messageBytes);
                event.words.fill(0);
                std::memcpy(event.words.data(), source + offset, messageBytes);
            } else
                dropped_plugin_output_events_.fetch_add(1, std::memory_order_relaxed);
            offset += messageBytes;
        }
    }

    void SequencerTrack::capturePresetRequest(int32_t instanceId, uint32_t index) {
        if (plugin_output_event_count_ == plugin_output_events_.size()) {
            dropped_plugin_output_events_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        auto& event = plugin_output_events_[plugin_output_event_count_++];
        event = {};
        event.instance_id = instanceId;
        event.preset_request = true;
        event.words[0] = index;
    }

    constexpr std::string_view kTrackGainNodeId = "builtin:track_gain";

    AudioGraphNodeDescriptor createTrackGainNodeDescriptor() {
        AudioGraphNodeDescriptor gainNode;
        gainNode.node_id = std::string(kTrackGainNodeId);
        gainNode.node_type = std::string(webaudio_compat::kGainNodeType);
        gainNode.display_name = "Track Volume";
        gainNode.parameters.emplace("gain", 1.0);
        return gainNode;
    }

    class SequencerTrackImpl : public SequencerTrack {
        bool bypass_{false};
        bool frozen_{false};
        std::atomic_bool muted_{false};
        std::atomic_bool solo_{false};
        std::unique_ptr<AudioPluginGraph> graph_;
        std::vector<int32_t> instance_ids{};
        std::unordered_map<int32_t, uint8_t> instance_groups_{}; // instanceId → UMP group

    public:
        explicit SequencerTrackImpl(std::unique_ptr<AudioPluginGraph>&& graph, size_t eventBufferSizeInBytes);
        ~SequencerTrackImpl() override = default;

        AudioPluginGraph& graph() override { return *graph_; }
        bool replaceGraph(std::unique_ptr<AudioPluginGraph>&& graph) override;
        uint32_t latencyInSamples() override { return graph_ ? graph_->mainOutputLatencyInSamples() : 0; }
        uint32_t renderLeadInSamples() override { return graph_ ? graph_->renderLeadInSamples() : 0; }
        double tailLengthInSeconds() override { return graph_ ? graph_->mainOutputTailLengthInSeconds() : 0.0; }
        double trackGain() const override;
        bool trackGain(double value) override;
        bool muted() const override { return muted_.load(std::memory_order_acquire); }
        void muted(bool value) override { muted_.store(value, std::memory_order_release); }
        bool solo() const override { return solo_.load(std::memory_order_acquire); }
        void solo(bool value) override { solo_.store(value, std::memory_order_release); }

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

    private:
        webaudio_compat::GainNode* findTrackGainNode() const;
        webaudio_compat::GainNode* ensureTrackGainNode();
    };

    SequencerTrackImpl::SequencerTrackImpl(std::unique_ptr<AudioPluginGraph>&& graph, size_t eventBufferSizeInBytes) :
        SequencerTrack(eventBufferSizeInBytes),
        graph_(std::move(graph)) {
    }

    std::unique_ptr<SequencerTrack> SequencerTrack::create(
        const AudioGraphProviderRegistry& registry,
        size_t eventBufferSizeInBytes,
        const std::string& graphProviderId) {
        auto graph = registry.createGraph(graphProviderId, eventBufferSizeInBytes);
        if (!graph) {
            // Only the default graph type may fall back. A caller that asked
            // for a named provider gets nothing rather than a lesser graph
            // silently standing in for the one it requested.
            if (!graphProviderId.empty())
                return nullptr;
            graph = AudioPluginGraph::create(eventBufferSizeInBytes);
            if (!graph)
                return nullptr;
        }
        graph->appendBuiltInNodeSimple(createTrackGainNodeDescriptor());
        return std::make_unique<SequencerTrackImpl>(std::move(graph), eventBufferSizeInBytes);
    }

    bool SequencerTrackImpl::replaceGraph(std::unique_ptr<AudioPluginGraph>&& graph) {
        if (!graph_ || !graph)
            return false;
        if (!AudioPluginGraph::migrate(*graph, *graph_))
            return false;
        graph_ = std::move(graph);
        ensureTrackGainNode();
        // Choosing a graph replaces whatever unloadable one this track was
        // standing in for, so there is nothing left to preserve.
        clearUnresolvedGraph();
        return true;
    }

    webaudio_compat::GainNode* SequencerTrackImpl::findTrackGainNode() const {
        if (!graph_)
            return nullptr;
        auto* node = graph_->getNode(std::string(kTrackGainNodeId));
        return dynamic_cast<webaudio_compat::GainNode*>(node);
    }

    webaudio_compat::GainNode* SequencerTrackImpl::ensureTrackGainNode() {
        if (auto* gain = findTrackGainNode())
            return gain;
        if (!graph_)
            return nullptr;
        if (graph_->appendBuiltInNodeSimple(createTrackGainNodeDescriptor()) != 0)
            return nullptr;
        return findTrackGainNode();
    }

    double SequencerTrackImpl::trackGain() const {
        auto* gain = findTrackGainNode();
        return gain ? gain->gain() : 1.0;
    }

    bool SequencerTrackImpl::trackGain(double value) {
        auto* gain = ensureTrackGainNode();
        if (!gain)
            return false;
        gain->gain(value);
        return true;
    }

}
