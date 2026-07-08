
#include "uapmd/uapmd.hpp"
#include "uapmd-graph/detail/node-graph/AudioPluginGraph.hpp"
#include "farbot/RealtimeObject.hpp"
#include "AudioPluginNodeImpl.hpp"

#include <cmath>
#include <limits>

namespace uapmd {

    using NodeList = std::vector<std::shared_ptr<AudioGraphNode>>;
    using RTNodeList = farbot::RealtimeObject<NodeList, farbot::RealtimeObjectOptions::nonRealtimeMutatable>;

    class AudioPluginGraphImpl : public AudioPluginGraph {
        RTNodeList nodes_;
        size_t event_buffer_size_in_bytes_;
        std::unique_ptr<AudioGraphRegistry> registry_;
        std::function<uint8_t(int32_t)> group_resolver_;
        std::function<void(int32_t, const uapmd_ump_t*, size_t)> event_output_callback_;

        uint32_t currentOutputBusCount();
        uint32_t aggregateLatencyInSamples();
        double aggregateTailLengthInSeconds();

    public:
        explicit AudioPluginGraphImpl(size_t eventBufferSizeInBytes, std::string providerId = {})
            : AudioPluginGraph(std::move(providerId))
            , event_buffer_size_in_bytes_(eventBufferSizeInBytes)
            , registry_(AudioGraphRegistry::createDefault()) {
        }
        ~AudioPluginGraphImpl() override = default;

        AudioGraphExtension* getExtension(const std::type_info& type) override;
        const AudioGraphExtension* getExtension(const std::type_info& type) const override;

        uapmd_status_t appendNodeSimple(int32_t instanceId, AudioPluginInstanceAPI* instance, std::function<void()>&& onDelete) override;
        uapmd_status_t appendBuiltInNodeSimple(const AudioGraphNodeDescriptor& descriptor) override;
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
        std::map<std::string, AudioGraphNode*> nodes() override;
        AudioGraphNode* getNode(const std::string& nodeId) override;
        std::map<int32_t, AudioPluginNode*> plugins() override;
        AudioPluginNode* getPluginNode(int32_t instanceId) override;
        std::vector<std::shared_ptr<AudioGraphNode>> releaseNodesForMigration() override;
        bool adoptNodesFromMigration(std::vector<std::shared_ptr<AudioGraphNode>>&& nodes) override;
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
            if (auto pluginNode = std::dynamic_pointer_cast<AudioPluginNodeImpl>(node)) {
                auto instanceId = pluginNode->instanceId();

                pluginNode->drainQueueToPending();

                uint8_t group = 0xFF;
                if (group_resolver_)
                    group = group_resolver_(instanceId);

                auto& eventIn = process.eventIn();
                if (pluginNode->consumeStopFlushRequest())
                    pluginNode->prepareStopFlush(eventIn, group);
                else
                    pluginNode->fillEventBufferForGroup(eventIn, group);

                bool bypassed = pluginNode->bypassed();
                if (!bypassed)
                    pluginNode->processInputMapping(process);
                if (bypassed) {
                    process.copyInputsToOutputs();
                    if (i + 1 < nodes.size())
                        process.advanceToNextNode();
                    continue;
                }

                // Keep the active-note bitmask in sync with what the plugin actually
                // receives, so a stop flush can emit genuine note-offs later.
                pluginNode->trackInputEvents(eventIn);

                auto status = pluginNode->processAudio(process);
                if (status != 0)
                    return status;

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
            } else {
                auto status = node->processAudio(process);
                if (status != 0)
                    return status;
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
            auto* buses = node->audioBuses();
            if (node->bypassed())
                continue;
            if (!buses)
                continue;
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
            if (node->bypassed())
                continue;
            total += node->latencyInSamples();
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
            if (node->bypassed())
                continue;
            const auto tail = node->tailLengthInSeconds();
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
        auto insertPos = std::find_if(access->begin(), access->end(), [](const auto& node) {
            return node && dynamic_cast<AudioPluginNode*>(node.get()) == nullptr;
        });
        access->insert(insertPos, std::move(newNode));
        return 0;
    }

    uapmd_status_t AudioPluginGraphImpl::appendBuiltInNodeSimple(const AudioGraphNodeDescriptor& descriptor) {
        if (descriptor.node_type.empty() || !registry_)
            return -1;
        if (getNode(descriptor.node_id))
            return -1;
        auto* factory = registry_->findBuiltInFactory(descriptor.node_type);
        if (!factory)
            return -1;
        auto newNode = factory->create(descriptor);
        if (!newNode)
            return -1;
        RTNodeList::ScopedAccess<farbot::ThreadType::nonRealtime> access(nodes_);
        access->push_back(std::move(newNode));
        return 0;
    }

    bool AudioPluginGraphImpl::removeNodeSimple(int32_t instanceId) {
        std::shared_ptr<AudioGraphNode> removed;
        {
            RTNodeList::ScopedAccess<farbot::ThreadType::nonRealtime> access(nodes_);
            auto& nodes = *access;
            for (size_t i = 0; i < nodes.size(); ++i) {
                auto pluginNode = std::dynamic_pointer_cast<AudioPluginNodeImpl>(nodes[i]);
                if (pluginNode && pluginNode->instanceId() == instanceId) {
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
            if (auto pluginNode = std::dynamic_pointer_cast<AudioPluginNodeImpl>(node))
                ret[pluginNode->instanceId()] = pluginNode.get();
        return ret;
    }

    std::map<std::string, AudioGraphNode*> AudioPluginGraphImpl::nodes() {
        RTNodeList::ScopedAccess<farbot::ThreadType::nonRealtime> access(nodes_);
        std::map<std::string, AudioGraphNode*> ret{};
        for (auto& node : *access)
            ret[node->nodeId()] = node.get();
        return ret;
    }

    AudioGraphNode* AudioPluginGraphImpl::getNode(const std::string& nodeId) {
        RTNodeList::ScopedAccess<farbot::ThreadType::nonRealtime> access(nodes_);
        for (auto& node : *access)
            if (node && node->nodeId() == nodeId)
                return node.get();
        return nullptr;
    }

    AudioPluginNode* AudioPluginGraphImpl::getPluginNode(int32_t instanceId) {
        RTNodeList::ScopedAccess<farbot::ThreadType::nonRealtime> access(nodes_);
        for (auto& node : *access)
            if (auto pluginNode = std::dynamic_pointer_cast<AudioPluginNodeImpl>(node);
                pluginNode && pluginNode->instanceId() == instanceId)
                return pluginNode.get();
        return nullptr;
    }

    std::vector<std::shared_ptr<AudioGraphNode>> AudioPluginGraphImpl::releaseNodesForMigration() {
        NodeList releasedNodes;
        {
            RTNodeList::ScopedAccess<farbot::ThreadType::nonRealtime> access(nodes_);
            releasedNodes = std::move(*access);
            access->clear();
        }

        std::vector<std::shared_ptr<AudioGraphNode>> result;
        result.reserve(releasedNodes.size());
        for (auto& node : releasedNodes)
            if (node)
                result.push_back(std::move(node));
        return result;
    }

    bool AudioPluginGraphImpl::adoptNodesFromMigration(std::vector<std::shared_ptr<AudioGraphNode>>&& nodes) {
        RTNodeList::ScopedAccess<farbot::ThreadType::nonRealtime> access(nodes_);
        for (auto& transferred : nodes) {
            if (!transferred)
                continue;
            access->push_back(std::move(transferred));
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
