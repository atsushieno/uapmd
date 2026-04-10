
#include "uapmd/uapmd.hpp"
#include "farbot/RealtimeObject.hpp"
#include "AudioPluginNodeImpl.hpp"

#include <cmath>
#include <limits>

namespace uapmd {

    using NodeList = std::vector<std::shared_ptr<AudioPluginNodeImpl>>;
    using RTNodeList = farbot::RealtimeObject<NodeList, farbot::RealtimeObjectOptions::nonRealtimeMutatable>;

    class AudioPluginGraphImpl : public AudioPluginGraph {
        RTNodeList nodes_;
        size_t event_buffer_size_in_bytes_;
        std::function<uint8_t(int32_t)> group_resolver_;
        std::function<void(int32_t, const uapmd_ump_t*, size_t)> event_output_callback_;

        uint32_t currentOutputBusCount();
        uint32_t aggregateLatencyInSamples();
        double aggregateTailLengthInSeconds();

    public:
        explicit AudioPluginGraphImpl(size_t eventBufferSizeInBytes, std::string providerId = {})
            : AudioPluginGraph(std::move(providerId))
            , event_buffer_size_in_bytes_(eventBufferSizeInBytes) {
        }
        ~AudioPluginGraphImpl() override = default;

        AudioGraphExtension* getExtension(const std::type_info& type) override;
        const AudioGraphExtension* getExtension(const std::type_info& type) const override;

        uapmd_status_t appendNodeSimple(int32_t instanceId, AudioPluginInstanceAPI* instance, std::function<void()>&& onDelete) override;
        bool removeNodeSimple(int32_t instanceId) override;
        void setGroupResolver(std::function<uint8_t(int32_t)> resolver) override;
        void setEventOutputCallback(std::function<void(int32_t, const uapmd_ump_t*, size_t)> callback) override;
        int32_t processAudio(AudioProcessContext& process) override;
        uint32_t outputBusCount() override;
        uint32_t outputLatencyInSamples(uint32_t outputBusIndex) override;
        double outputTailLengthInSeconds(uint32_t outputBusIndex) override;
        uint32_t renderLeadInSamples() override;
        uint32_t mainOutputLatencyInSamples() override;
        double mainOutputTailLengthInSeconds() override;
        std::map<int32_t, AudioPluginNode*> plugins() override;
        AudioPluginNode* getPluginNode(int32_t instanceId) override;
        std::vector<std::shared_ptr<AudioPluginNode>> releaseNodesForMigration() override;
        bool adoptNodesFromMigration(std::vector<std::shared_ptr<AudioPluginNode>>&& nodes) override;
    };

    AudioGraphExtension* AudioPluginGraphImpl::getExtension(const std::type_info& type) {
        (void) type;
        return nullptr;
    }

    const AudioGraphExtension* AudioPluginGraphImpl::getExtension(const std::type_info& type) const {
        (void) type;
        return nullptr;
    }

    void AudioPluginGraphImpl::setGroupResolver(std::function<uint8_t(int32_t)> resolver) {
        group_resolver_ = std::move(resolver);
    }

    void AudioPluginGraphImpl::setEventOutputCallback(std::function<void(int32_t, const uapmd_ump_t*, size_t)> callback) {
        event_output_callback_ = std::move(callback);
    }

    int32_t AudioPluginGraphImpl::processAudio(AudioProcessContext& process) {
        RTNodeList::ScopedAccess<farbot::ThreadType::realtime> access(nodes_);
        auto& nodes = *access;

        if (nodes.empty()) {
            // No plugins to process, simply route timeline/device input straight to output.
            process.copyInputsToOutputs();
            return 0;
        }

        process.clearAudioOutputs();

        for (size_t i = 0; i < nodes.size(); ++i) {
            auto& node = nodes[i];
            if (!node)
                continue;
            auto instanceId = node->instanceId();

            // Drain queue to pending events
            node->drainQueueToPending();

            // Get group for this instance
            uint8_t group = 0xFF;
            if (group_resolver_)
                group = group_resolver_(instanceId);

            // Fill event buffer with events for this group
            auto& eventIn = process.eventIn();
            if (node->consumeStopFlushRequest())
                node->prepareStopFlush(eventIn, group);
            else
                node->fillEventBufferForGroup(eventIn, group);

            bool bypassed = node->instance()->bypassed();
            if (!bypassed)
                node->processInputMapping(process);
            if (bypassed) {
                // Pass audio through regardless of position so the signal is
                // preserved when a plugin is disabled.  For a synthesizer that
                // is the sole node on a track, inputs are empty and
                // copyInputsToOutputs() produces silence — which is correct.
                process.copyInputsToOutputs();
                if (i + 1 < nodes.size())
                    process.advanceToNextNode();
                continue;
            }

            // Process audio
            auto status = node->instance()->processAudio(process);
            if (status != 0)
                return status;

            // Handle event output
            auto& eventOut = process.eventOut();
            if (eventOut.position() > 0) {
                if (event_output_callback_) {
                    event_output_callback_(
                        instanceId,
                        static_cast<uapmd_ump_t*>(eventOut.getMessages()),
                        eventOut.position()
                    );
                }
                eventOut.position(0);
            }

            if (i + 1 < nodes.size())
                process.advanceToNextNode();
        }
        return 0;
    }

    uint32_t AudioPluginGraphImpl::currentOutputBusCount() {
        RTNodeList::ScopedAccess<farbot::ThreadType::nonRealtime> access(nodes_);
        for (auto it = access->rbegin(); it != access->rend(); ++it) {
            const auto& node = *it;
            if (!node)
                continue;
            auto* instance = node->instance();
            if (!instance || instance->bypassed())
                continue;
            auto* buses = instance->audioBuses();
            if (!buses)
                break;
            uint32_t enabledOutputs = 0;
            for (auto* bus : buses->audioOutputBuses())
                if (bus && bus->enabled())
                    ++enabledOutputs;
            if (enabledOutputs > 0)
                return enabledOutputs;
            break;
        }
        return 1;
    }

    uint32_t AudioPluginGraphImpl::aggregateLatencyInSamples() {
        RTNodeList::ScopedAccess<farbot::ThreadType::nonRealtime> access(nodes_);
        uint64_t total = 0;
        for (const auto& node : *access) {
            if (!node)
                continue;
            auto* instance = node->instance();
            if (!instance || instance->bypassed())
                continue;
            total += instance->latencyInSamples();
        }
        return total > std::numeric_limits<uint32_t>::max() ?
            std::numeric_limits<uint32_t>::max() :
            static_cast<uint32_t>(total);
    }

    double AudioPluginGraphImpl::aggregateTailLengthInSeconds() {
        RTNodeList::ScopedAccess<farbot::ThreadType::nonRealtime> access(nodes_);
        double total = 0.0;
        for (const auto& node : *access) {
            if (!node)
                continue;
            auto* instance = node->instance();
            if (!instance || instance->bypassed())
                continue;
            const auto tail = instance->tailLengthInSeconds();
            if (std::isinf(tail))
                return std::numeric_limits<double>::infinity();
            total += std::max(0.0, tail);
        }
        return total;
    }

    uint32_t AudioPluginGraphImpl::outputBusCount() {
        return currentOutputBusCount();
    }

    uint32_t AudioPluginGraphImpl::outputLatencyInSamples(uint32_t outputBusIndex) {
        if (outputBusIndex >= currentOutputBusCount())
            return 0;
        return aggregateLatencyInSamples();
    }

    double AudioPluginGraphImpl::outputTailLengthInSeconds(uint32_t outputBusIndex) {
        if (outputBusIndex >= currentOutputBusCount())
            return 0.0;
        return aggregateTailLengthInSeconds();
    }

    uint32_t AudioPluginGraphImpl::renderLeadInSamples() {
        uint32_t maxLatency = 0;
        const auto count = outputBusCount();
        for (uint32_t outputBusIndex = 0; outputBusIndex < count; ++outputBusIndex)
            maxLatency = std::max(maxLatency, outputLatencyInSamples(outputBusIndex));
        return maxLatency;
    }

    uint32_t AudioPluginGraphImpl::mainOutputLatencyInSamples() {
        return outputLatencyInSamples(0);
    }

    double AudioPluginGraphImpl::mainOutputTailLengthInSeconds() {
        return outputTailLengthInSeconds(0);
    }

    uapmd_status_t AudioPluginGraphImpl::appendNodeSimple(int32_t instanceId, AudioPluginInstanceAPI* instance, std::function<void()>&& onDelete) {
        auto newNode = std::make_shared<AudioPluginNodeImpl>(instanceId, instance, event_buffer_size_in_bytes_, std::move(onDelete));
        RTNodeList::ScopedAccess<farbot::ThreadType::nonRealtime> access(nodes_);
        access->push_back(std::move(newNode));
        // FIXME: define return codes
        return 0;
    }

    bool AudioPluginGraphImpl::removeNodeSimple(int32_t instanceId) {
        std::shared_ptr<AudioPluginNodeImpl> removed;
        {
            RTNodeList::ScopedAccess<farbot::ThreadType::nonRealtime> access(nodes_);
            auto& nodes = *access;
            for (size_t i = 0; i < nodes.size(); ++i) {
                if (nodes[i]->instanceId() == instanceId) {
                    removed = std::move(nodes[i]);
                    nodes.erase(nodes.begin() + static_cast<ptrdiff_t>(i));
                    break;
                }
            }
        }
        // ~AudioPluginNodeImpl (and on_delete_()) runs here, outside the farbot lock.
        return removed != nullptr;
    }

    std::map<int32_t, AudioPluginNode*> AudioPluginGraphImpl::plugins() {
        RTNodeList::ScopedAccess<farbot::ThreadType::nonRealtime> access(nodes_);
        std::map<int32_t, AudioPluginNode*> ret{};
        for (auto& node : *access)
            ret[node->instanceId()] = node.get();
        return ret;
    }

    AudioPluginNode* AudioPluginGraphImpl::getPluginNode(int32_t instanceId) {
        RTNodeList::ScopedAccess<farbot::ThreadType::nonRealtime> access(nodes_);
        for (auto& node : *access)
            if (node->instanceId() == instanceId)
                return node.get();
        return nullptr;
    }

    std::vector<std::shared_ptr<AudioPluginNode>> AudioPluginGraphImpl::releaseNodesForMigration() {
        NodeList releasedNodes;
        {
            RTNodeList::ScopedAccess<farbot::ThreadType::nonRealtime> access(nodes_);
            releasedNodes = std::move(*access);
            access->clear();
        }

        std::vector<std::shared_ptr<AudioPluginNode>> result;
        result.reserve(releasedNodes.size());
        for (auto& node : releasedNodes)
            if (node)
                result.push_back(std::move(node));
        return result;
    }

    bool AudioPluginGraphImpl::adoptNodesFromMigration(std::vector<std::shared_ptr<AudioPluginNode>>&& nodes) {
        RTNodeList::ScopedAccess<farbot::ThreadType::nonRealtime> access(nodes_);
        for (auto& transferred : nodes) {
            auto node = std::dynamic_pointer_cast<AudioPluginNodeImpl>(transferred);
            if (!node)
                return false;
            access->push_back(std::move(node));
        }
        return true;
    }

    std::unique_ptr<AudioPluginGraph> AudioPluginGraph::create(size_t eventBufferSizeInBytes) {
        return std::make_unique<AudioPluginGraphImpl>(eventBufferSizeInBytes);
    }

    bool AudioPluginGraph::migrate(AudioPluginGraph& to, AudioPluginGraph& from) {
        auto nodes = from.releaseNodesForMigration();
        if (!to.adoptNodesFromMigration(std::move(nodes)))
            return false;
        return true;
    }

}
