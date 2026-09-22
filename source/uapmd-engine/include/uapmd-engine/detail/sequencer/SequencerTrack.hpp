#pragma once
#include <cstdint>
#include <array>
#include <atomic>
#include <functional>
#include <string>
#include <memory>
#include <vector>
#include <span>

#include "uapmd-midi-service/uapmd-midi-service.hpp"
#include "uapmd-graph/uapmd-graph.hpp"

namespace uapmd {

    struct TrackPluginOutputEvent {
        int32_t instance_id{};
        uint32_t size_in_bytes{};
        bool preset_request{}; // When true, words[0] is a preset index, not UMP.
        std::array<uapmd_ump_t, 4> words{};
    };

    class SequencerTrack {
        std::string unresolved_graph_type_{};
        std::vector<uint8_t> unresolved_graph_payload_{};
        std::vector<TrackPluginOutputEvent> plugin_output_events_;
        size_t plugin_output_event_count_{};
        std::atomic<uint32_t> dropped_plugin_output_events_{0};
        std::vector<std::pair<int32_t, uint8_t>> processing_groups_;

    protected:
        SequencerTrack() = default;
        explicit SequencerTrack(size_t eventBufferSizeInBytes);

    public:
        virtual ~SequencerTrack() = default;

        // One processing thread owns this preallocated buffer at a time. The
        // coordinator may read/clear it only after graph processing completes.
        // Capture copies complete UMP messages and drops newest on overflow.
        void capturePluginOutput(int32_t instanceId, const uapmd_ump_t* data, size_t bytes);
        // Preset requests share the bounded event storage and overflow counter.
        void capturePresetRequest(int32_t instanceId, uint32_t index);
        std::span<const TrackPluginOutputEvent> pluginOutputEvents() const { return {plugin_output_events_.data(), plugin_output_event_count_}; }
        void clearPluginOutputEvents() { plugin_output_event_count_ = 0; }
        // Lifetime counter, wraps modulo 2^32; readable from the control thread.
        uint32_t droppedPluginOutputEventCount() const { return dropped_plugin_output_events_.load(std::memory_order_relaxed); }

        // Configure off the processing thread. Refresh values on the coordinator
        // before dispatch; graphs only read this cache while workers are active.
        void configureProcessingGroups() {
            processing_groups_.clear();
            for (const auto instanceId : orderedInstanceIds())
                processing_groups_.emplace_back(instanceId, 0xFF);
        }
        void setProcessingGroup(int32_t instanceId, uint8_t group) {
            for (auto& entry : processing_groups_)
                if (entry.first == instanceId) {
                    entry.second = group;
                    return;
                }
        }
        uint8_t processingGroup(int32_t instanceId) const {
            for (const auto& entry : processing_groups_)
                if (entry.first == instanceId)
                    return entry.second;
            return 0xFF;
        }

        // A graph this build could not construct, because no provider claimed
        // its type when the project loaded -- typically an addin that is
        // absent or disabled. The track runs a substitute graph; these carry
        // the original definition verbatim so that saving writes it back
        // rather than replacing it with the substitute. Both are empty for
        // every track whose graph did load.
        //
        // Concrete state rather than virtuals: the meaning is the same for
        // every track implementation, as with `AudioGraph::providerId()`.
        const std::string& unresolvedGraphType() const { return unresolved_graph_type_; }
        const std::vector<uint8_t>& unresolvedGraphPayload() const { return unresolved_graph_payload_; }
        void unresolvedGraph(std::string graphType, std::vector<uint8_t> payload) {
            unresolved_graph_type_ = std::move(graphType);
            unresolved_graph_payload_ = std::move(payload);
        }
        void clearUnresolvedGraph() {
            unresolved_graph_type_.clear();
            unresolved_graph_payload_.clear();
        }
        static std::unique_ptr<SequencerTrack> create(
            const AudioGraphProviderRegistry& registry,
            size_t eventBufferSizeInBytes,
            const std::string& graphProviderId);

        virtual uapmd_graph::AudioPluginGraph& graph() = 0;
        virtual bool replaceGraph(std::unique_ptr<uapmd_graph::AudioPluginGraph>&& graph) = 0;
        virtual uint32_t latencyInSamples() = 0;
        virtual uint32_t renderLeadInSamples() = 0;
        virtual double tailLengthInSeconds() = 0;
        virtual double trackGain() const = 0;
        virtual bool trackGain(double value) = 0;
        // These control admission to the main mix without stopping the graph.
        virtual bool muted() const = 0;
        virtual void muted(bool value) = 0;
        virtual bool solo() const = 0;
        virtual void solo(bool value) = 0;
        virtual std::vector<int32_t>& orderedInstanceIds() = 0;

        virtual bool bypassed() = 0;
        virtual bool frozen() = 0;
        virtual void bypassed(bool value) = 0;
        virtual void frozen(bool value) = 0;

        // UMP group assignment per plugin instance (0–15).
        // findAvailableGroup returns the lowest unused group, or 0xFF if all 16 are taken.
        virtual void    setInstanceGroup(int32_t instanceId, uint8_t group) = 0;
        virtual uint8_t getInstanceGroup(int32_t instanceId) const = 0; // 0xFF = unknown
        virtual uint8_t findAvailableGroup() const = 0;
        virtual void removeInstance(int32_t instanceId) = 0;
    };

}
