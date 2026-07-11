#include "uapmd/uapmd.hpp"
#include "uapmd-graph/detail/node-graph/AudioPluginFullDAGraph.hpp"
#include "uapmd-graph/detail/node-graph/AudioBusesLayoutExtension.hpp"
#include "uapmd-graph/detail/node-graph/OutputRoutingExtension.hpp"
#include "uapmd-graph/detail/node-graph/AudioGraphRegistry.hpp"
#include "farbot/RealtimeObject.hpp"
#include "AudioPluginNodeImpl.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <queue>
#include <ranges>
#include <sstream>
#include <unordered_map>

namespace uapmd {

    namespace {

        constexpr size_t kGraphScratchCapacityFrames = 16384;
        constexpr uint32_t kDefaultBuiltInChannelCount = 2;

        using NodePtr = std::shared_ptr<AudioPluginNodeImpl>;

        std::string defaultNodeIdForInstance(int32_t instanceId) {
            return "plugin:" + std::to_string(instanceId);
        }

        uint32_t pluginAudioInputBusCount(AudioPluginInstanceAPI* instance) {
            if (!instance || !instance->audioBuses())
                return 0;
            uint32_t count = 0;
            for (auto* bus : instance->audioBuses()->audioInputBuses())
                if (bus && bus->enabled())
                    ++count;
            return count;
        }

        uint32_t pluginAudioOutputBusCount(AudioPluginInstanceAPI* instance) {
            if (!instance || !instance->audioBuses())
                return 0;
            uint32_t count = 0;
            for (auto* bus : instance->audioBuses()->audioOutputBuses())
                if (bus && bus->enabled())
                    ++count;
            return count;
        }

        uint32_t pluginEventInputBusCount(AudioPluginInstanceAPI* instance) {
            return (instance && instance->audioBuses() && instance->audioBuses()->hasEventInputs()) ? 1u : 0u;
        }

        uint32_t pluginEventOutputBusCount(AudioPluginInstanceAPI* instance) {
            return (instance && instance->audioBuses() && instance->audioBuses()->hasEventOutputs()) ? 1u : 0u;
        }

        std::vector<remidy::AudioBusSpec> buildAudioBusSpecs(
            const std::vector<remidy::AudioBusConfiguration*>& buses,
            size_t bufferSizeInFrames) {
            std::vector<remidy::AudioBusSpec> specs;
            for (auto* bus : buses) {
                if (!bus || !bus->enabled())
                    continue;
                specs.push_back(remidy::AudioBusSpec{
                    bus->role(),
                    bus->channelLayout().channels(),
                    bufferSizeInFrames
                });
            }
            return specs;
        }

        void clearAudioInputs(remidy::AudioProcessContext& process) {
            auto dataType = process.masterContext().audioDataType();
            for (int32_t bus = 0; bus < process.audioInBusCount(); ++bus) {
                const size_t frames = std::min(
                    static_cast<size_t>(std::max(process.frameCount(), 0)),
                    process.inputBusBufferCapacityInFrames(bus));
                for (uint32_t ch = 0; ch < static_cast<uint32_t>(process.inputChannelCount(bus)); ++ch) {
                    if (dataType == remidy::AudioContentType::Float64) {
                        if (auto* dst = process.getDoubleInBuffer(bus, ch))
                            std::memset(dst, 0, frames * sizeof(double));
                    } else {
                        if (auto* dst = process.getFloatInBuffer(bus, ch))
                            std::memset(dst, 0, frames * sizeof(float));
                    }
                }
            }
            process.eventIn().position(0);
        }

        void syncMasterContext(remidy::MasterContext& dst, remidy::MasterContext& src) {
            dst.audioDataType(src.audioDataType());
            dst.deltaClockstampTicksPerQuarterNotes(src.deltaClockstampTicksPerQuarterNotes());
            dst.tempo(src.tempo());
            dst.playbackPositionSamples(src.playbackPositionSamples());
            dst.sampleRate(src.sampleRate());
            dst.isPlaying(src.isPlaying());
            dst.timeSignatureNumerator(src.timeSignatureNumerator());
            dst.timeSignatureDenominator(src.timeSignatureDenominator());
        }

        void accumulateAudioBus(remidy::AudioProcessContext& dst,
                                bool dstIsInput,
                                uint32_t dstBusIndex,
                                remidy::AudioProcessContext& src,
                                bool srcIsInput,
                                uint32_t srcBusIndex) {
            auto dataType = dst.masterContext().audioDataType();
            const int32_t dstBusCount = dstIsInput ? dst.audioInBusCount() : dst.audioOutBusCount();
            const int32_t srcBusCount = srcIsInput ? src.audioInBusCount() : src.audioOutBusCount();
            if (dstBusIndex >= static_cast<uint32_t>(dstBusCount) || srcBusIndex >= static_cast<uint32_t>(srcBusCount))
                return;

            const size_t frames = std::min(
                static_cast<size_t>(std::max(dst.frameCount(), 0)),
                std::min(
                    dstIsInput ? dst.inputBusBufferCapacityInFrames(static_cast<int32_t>(dstBusIndex))
                               : dst.outputBusBufferCapacityInFrames(static_cast<int32_t>(dstBusIndex)),
                    srcIsInput ? src.inputBusBufferCapacityInFrames(static_cast<int32_t>(srcBusIndex))
                               : src.outputBusBufferCapacityInFrames(static_cast<int32_t>(srcBusIndex))));

            const uint32_t channels = std::min(
                dstIsInput ? static_cast<uint32_t>(dst.inputChannelCount(static_cast<int32_t>(dstBusIndex)))
                           : static_cast<uint32_t>(dst.outputChannelCount(static_cast<int32_t>(dstBusIndex))),
                srcIsInput ? static_cast<uint32_t>(src.inputChannelCount(static_cast<int32_t>(srcBusIndex)))
                           : static_cast<uint32_t>(src.outputChannelCount(static_cast<int32_t>(srcBusIndex))));

            for (uint32_t ch = 0; ch < channels; ++ch) {
                if (dataType == remidy::AudioContentType::Float64) {
                    auto* dstBuf = dstIsInput ? dst.getDoubleInBuffer(static_cast<int32_t>(dstBusIndex), ch)
                                              : dst.getDoubleOutBuffer(static_cast<int32_t>(dstBusIndex), ch);
                    auto* srcBuf = srcIsInput ? src.getDoubleInBuffer(static_cast<int32_t>(srcBusIndex), ch)
                                              : src.getDoubleOutBuffer(static_cast<int32_t>(srcBusIndex), ch);
                    if (!dstBuf || !srcBuf)
                        continue;
                    for (size_t frame = 0; frame < frames; ++frame)
                        dstBuf[frame] += srcBuf[frame];
                } else {
                    auto* dstBuf = dstIsInput ? dst.getFloatInBuffer(static_cast<int32_t>(dstBusIndex), ch)
                                              : dst.getFloatOutBuffer(static_cast<int32_t>(dstBusIndex), ch);
                    auto* srcBuf = srcIsInput ? src.getFloatInBuffer(static_cast<int32_t>(srcBusIndex), ch)
                                              : src.getFloatOutBuffer(static_cast<int32_t>(srcBusIndex), ch);
                    if (!dstBuf || !srcBuf)
                        continue;
                    for (size_t frame = 0; frame < frames; ++frame)
                        dstBuf[frame] += srcBuf[frame];
                }
            }
        }

        void copyInputToOutput(remidy::AudioProcessContext& dst,
                               uint32_t dstBusIndex,
                               remidy::AudioProcessContext& src,
                               uint32_t srcBusIndex) {
            accumulateAudioBus(dst, false, dstBusIndex, src, true, srcBusIndex);
        }

        void copyInputToInput(remidy::AudioProcessContext& dst,
                              uint32_t dstBusIndex,
                              remidy::AudioProcessContext& src,
                              uint32_t srcBusIndex) {
            accumulateAudioBus(dst, true, dstBusIndex, src, true, srcBusIndex);
        }

        bool appendEventBytes(remidy::EventSequence& dst, const uint8_t* data, size_t size) {
            if (!data || size == 0)
                return true;
            if (dst.position() + size > dst.maxMessagesInBytes())
                return false;
            std::memcpy(static_cast<uint8_t*>(dst.getMessages()) + dst.position(), data, size);
            dst.position(dst.position() + size);
            return true;
        }

        bool appendEventsForGroup(remidy::EventSequence& dst, remidy::EventSequence& src, uint8_t group) {
            auto* bytes = static_cast<const uint8_t*>(src.getMessages());
            size_t offset = 0;
            while (offset + sizeof(uint32_t) <= src.position()) {
                auto* words = reinterpret_cast<const uint32_t*>(bytes + offset);
                const auto messageType = static_cast<uint8_t>(words[0] >> 28);
                const auto wordCount = umppi::umpSizeInInts(messageType);
                const auto size = static_cast<size_t>(wordCount) * sizeof(uint32_t);
                if (offset + size > src.position())
                    break;
                const auto msgGroup = static_cast<uint8_t>((words[0] >> 24) & 0x0F);
                if (group == 0xFF || msgGroup == group)
                    if (!appendEventBytes(dst, bytes + offset, size))
                        return false;
                offset += size;
            }
            return true;
        }

        struct GraphNodeRuntime {
            std::shared_ptr<AudioGraphNode> node;
            remidy::MasterContext master_context{};
            remidy::AudioProcessContext process;
            std::vector<AudioPluginGraphConnection> incoming_audio{};
            std::vector<AudioPluginGraphConnection> incoming_event{};

            GraphNodeRuntime(const std::shared_ptr<AudioGraphNode>& nodeRef, size_t eventBufferSizeInBytes)
                : node(nodeRef), process(master_context, static_cast<uint32_t>(eventBufferSizeInBytes)) {}
        };

        struct GraphState {
            std::vector<NodePtr> nodes{};
            std::vector<std::shared_ptr<AudioGraphNode>> builtin_nodes{};
            std::unordered_map<std::string, std::shared_ptr<GraphNodeRuntime>> runtimes{};
            std::vector<std::string> topo_order{};
            std::vector<AudioPluginGraphConnection> connections{};
            std::vector<AudioPluginGraphConnection> output_audio_links{};
            std::vector<AudioPluginGraphConnection> output_event_links{};
            AudioGraphBusesLayout runtime_layout{};
            std::vector<uint32_t> output_latencies{};
            std::vector<double> output_tails{};
            bool custom_topology{false};
            bool has_cycle{false};
            int64_t next_connection_id{1};
        };

        using RTGraphState = farbot::RealtimeObject<GraphState, farbot::RealtimeObjectOptions::nonRealtimeMutatable>;

        bool isPluginEndpoint(const AudioPluginGraphEndpoint& endpoint) {
            return endpoint.type == AudioPluginGraphEndpointType::Plugin;
        }

        std::string endpointNodeId(const AudioPluginGraphEndpoint& endpoint) {
            if (!endpoint.node_id.empty())
                return endpoint.node_id;
            if (endpoint.type != AudioPluginGraphEndpointType::Plugin || endpoint.instance_id < 0)
                return {};
            return defaultNodeIdForInstance(endpoint.instance_id);
        }

        bool isValidEndpointDirection(const AudioPluginGraphConnection& connection) {
            if (connection.source.type == AudioPluginGraphEndpointType::GraphOutput)
                return false;
            if (connection.target.type == AudioPluginGraphEndpointType::GraphInput)
                return false;
        if (connection.source.type == AudioPluginGraphEndpointType::GraphInput &&
            connection.target.type == AudioPluginGraphEndpointType::GraphOutput)
            return true;
        return true;
        }

        std::shared_ptr<AudioGraphNode> findNodeById(const GraphState& state, const std::string& nodeId) {
            if (nodeId.empty())
                return nullptr;
            for (const auto& node : state.nodes)
                if (node && node->nodeId() == nodeId)
                    return node;
            for (const auto& node : state.builtin_nodes)
                if (node && node->nodeId() == nodeId)
                    return node;
            return nullptr;
        }

        std::vector<std::shared_ptr<AudioGraphNode>> allNodes(const GraphState& state) {
            std::vector<std::shared_ptr<AudioGraphNode>> result;
            result.reserve(state.nodes.size() + state.builtin_nodes.size());
            for (const auto& node : state.nodes)
                if (node)
                    result.push_back(node);
            for (const auto& node : state.builtin_nodes)
                if (node)
                    result.push_back(node);
            return result;
        }

        uint32_t nodeInputBusCount(const GraphState& state, const std::shared_ptr<AudioGraphNode>& node, AudioPluginGraphBusType busType) {
            if (!node)
                return 0;
            if (auto* pluginNode = dynamic_cast<AudioPluginNode*>(node.get())) {
                auto* instance = pluginNode->instance();
                if (!instance)
                    return 0;
                return busType == AudioPluginGraphBusType::Audio
                    ? pluginAudioInputBusCount(instance)
                    : pluginEventInputBusCount(instance);
            }
            if (busType == AudioPluginGraphBusType::Audio) {
                auto counts = node->requiredAudioInputChannelCounts();
                if (!counts.empty())
                    return static_cast<uint32_t>(counts.size());
                return state.runtime_layout.audio_input_bus_count;
            }
            return state.runtime_layout.event_input_bus_count;
        }

        uint32_t nodeOutputBusCount(const GraphState& state, const std::shared_ptr<AudioGraphNode>& node, AudioPluginGraphBusType busType) {
            if (!node)
                return 0;
            if (auto* pluginNode = dynamic_cast<AudioPluginNode*>(node.get())) {
                auto* instance = pluginNode->instance();
                if (!instance)
                    return 0;
                return busType == AudioPluginGraphBusType::Audio
                    ? pluginAudioOutputBusCount(instance)
                    : pluginEventOutputBusCount(instance);
            }
            if (busType == AudioPluginGraphBusType::Audio) {
                auto counts = node->requiredAudioOutputChannelCounts();
                if (!counts.empty())
                    return static_cast<uint32_t>(counts.size());
                return state.runtime_layout.audio_output_bus_count;
            }
            return state.runtime_layout.event_output_bus_count;
        }

        constexpr auto kOutputRoutingPropertyKey = "output_routing";

        const char* outputRoutingTypeToString(TrackOutputRoutingTargetType type) {
            switch (type) {
                case TrackOutputRoutingTargetType::MASTER_INPUT_BUS:
                    return "master_input_bus";
                case TrackOutputRoutingTargetType::MAIN_MIX_BUS:
                    return "main_mix_bus";
                case TrackOutputRoutingTargetType::DISABLED:
                default:
                    return "disabled";
            }
        }

        TrackOutputRoutingTargetType parseOutputRoutingType(std::string_view value) {
            if (value == "master_input_bus")
                return TrackOutputRoutingTargetType::MASTER_INPUT_BUS;
            if (value == "main_mix_bus")
                return TrackOutputRoutingTargetType::MAIN_MIX_BUS;
            return TrackOutputRoutingTargetType::DISABLED;
        }

        std::string serializeOutputRoutingRules(const std::vector<TrackOutputRoutingRule>& rules) {
            std::ostringstream stream;
            for (size_t i = 0; i < rules.size(); ++i) {
                const auto& rule = rules[i];
                if (i > 0)
                    stream << ';';
                stream << rule.source_bus_index
                       << ','
                       << outputRoutingTypeToString(rule.target.type)
                       << ','
                       << rule.target.bus_index;
            }
            return stream.str();
        }

        std::vector<TrackOutputRoutingRule> parseOutputRoutingRules(const std::string& text) {
            std::vector<TrackOutputRoutingRule> rules;
            std::stringstream stream(text);
            std::string item;
            while (std::getline(stream, item, ';')) {
                if (item.empty())
                    continue;
                std::stringstream itemStream(item);
                std::string sourceBusIndexText, typeText, targetBusIndexText;
                if (!std::getline(itemStream, sourceBusIndexText, ',') ||
                    !std::getline(itemStream, typeText, ',') ||
                    !std::getline(itemStream, targetBusIndexText, ','))
                    continue;
                try {
                    TrackOutputRoutingRule rule;
                    rule.source_bus_index = static_cast<uint32_t>(std::stoul(sourceBusIndexText));
                    rule.target.type = parseOutputRoutingType(typeText);
                    rule.target.bus_index = static_cast<uint32_t>(std::stoul(targetBusIndexText));
                    rules.push_back(rule);
                } catch (...) {
                }
            }
            return rules;
        }

    } // namespace

    class AudioPluginFullDAGraphImpl : public AudioPluginFullDAGraph, public AudioBusesLayoutExtension, public OutputRoutingExtension {
        RTGraphState state_;
        std::unique_ptr<AudioGraphRegistry> registry_;
        size_t event_buffer_size_in_bytes_;
        std::function<uint8_t(int32_t)> group_resolver_;
        std::function<void(int32_t, const uapmd_ump_t*, size_t)> event_output_callback_;
        std::vector<TrackOutputRoutingRule> output_routing_rules_{};

        NodePtr findNode(const GraphState& state, int32_t instanceId) const;
        bool endpointExists(const GraphState& state, const AudioPluginGraphEndpoint& endpoint, AudioPluginGraphBusType busType) const;
        void rebuildCompiledState(GraphState& state) const;
        void rebuildSimpleConnections(GraphState& state) const;
        uint8_t resolveGroup(int32_t instanceId) const;

    public:
        explicit AudioPluginFullDAGraphImpl(size_t eventBufferSizeInBytes, std::string providerId)
            : AudioPluginFullDAGraph(std::move(providerId))
            , registry_(AudioGraphRegistry::createDefault())
            , event_buffer_size_in_bytes_(eventBufferSizeInBytes) {
            RTGraphState::ScopedAccess<farbot::ThreadType::nonRealtime> access(state_);
            rebuildSimpleConnections(*access);
        }

        ~AudioPluginFullDAGraphImpl() override = default;

        AudioGraphExtension* getExtension(const std::type_info& type) override;
        const AudioGraphExtension* getExtension(const std::type_info& type) const override;

        uapmd_status_t appendNodeSimple(int32_t instanceId, AudioPluginInstanceAPI* instance, std::function<void()>&& onDelete) override;
        uapmd_status_t appendBuiltInNodeSimple(const AudioGraphNodeDescriptor& descriptor) override;
        bool removeNodeSimple(int32_t instanceId) override;
        std::map<std::string, AudioGraphNode*> nodes() override;
        AudioGraphNode* getNode(const std::string& nodeId) override;
        std::map<int32_t, AudioPluginNode*> plugins() override;
        AudioPluginNode* getPluginNode(int32_t instanceId) override;
        std::vector<AudioPluginGraphConnection> connections() override;
        void refreshTimingInfo() override;
        void saveTo(std::map<std::string, std::string>& entries) override;
        void loadFrom(const std::map<std::string, std::string>& entries) override;
        std::vector<TrackOutputRoutingRule> outputRoutingRules() const override;
        void outputRoutingRules(const std::vector<TrackOutputRoutingRule>& rules) override;
        TrackOutputRoutingTarget outputRoutingTarget(uint32_t outputBusIndex) const override;
        uapmd_status_t connect(const AudioPluginGraphConnection& connection) override;
        bool disconnect(int64_t connectionId) override;
        void clearConnections() override;
        std::vector<std::shared_ptr<AudioGraphNode>> releaseNodesForMigration() override;
        bool adoptNodesFromMigration(std::vector<std::shared_ptr<AudioGraphNode>>&& nodes) override;
        AudioGraphBusesLayout busesLayout() override;
        void applyBusesLayout(const AudioGraphBusesLayout& layout) override;
        void setGroupResolver(std::function<uint8_t(int32_t)> resolver) override;
        void setEventOutputCallback(std::function<void(int32_t, const uapmd_ump_t*, size_t)> callback) override;
        int32_t processAudio(AudioProcessContext& process) override;
        uint32_t outputBusCount() override;
        uint32_t outputLatencyInSamples(uint32_t outputBusIndex) override;
        double outputTailLengthInSeconds(uint32_t outputBusIndex) override;
        uint32_t renderLeadInSamples() override;
        uint32_t mainOutputLatencyInSamples() override;
        double mainOutputTailLengthInSeconds() override;
    };

    AudioGraphExtension* AudioPluginFullDAGraphImpl::getExtension(const std::type_info& type) {
        if (type == typeid(AudioBusesLayoutExtension))
            return static_cast<AudioBusesLayoutExtension*>(this);
        if (type == typeid(OutputRoutingExtension))
            return static_cast<OutputRoutingExtension*>(this);
        return nullptr;
    }

    const AudioGraphExtension* AudioPluginFullDAGraphImpl::getExtension(const std::type_info& type) const {
        if (type == typeid(AudioBusesLayoutExtension))
            return static_cast<const AudioBusesLayoutExtension*>(this);
        if (type == typeid(OutputRoutingExtension))
            return static_cast<const OutputRoutingExtension*>(this);
        return nullptr;
    }

    NodePtr AudioPluginFullDAGraphImpl::findNode(const GraphState& state, int32_t instanceId) const {
        auto it = std::ranges::find_if(state.nodes, [instanceId](const NodePtr& node) {
            return node && node->instanceId() == instanceId;
        });
        return it == state.nodes.end() ? nullptr : *it;
    }

    bool AudioPluginFullDAGraphImpl::endpointExists(const GraphState& state,
                                              const AudioPluginGraphEndpoint& endpoint,
                                              AudioPluginGraphBusType busType) const {
        switch (endpoint.type) {
            case AudioPluginGraphEndpointType::GraphInput:
                return busType == AudioPluginGraphBusType::Audio
                    ? endpoint.bus_index < state.runtime_layout.audio_input_bus_count
                    : endpoint.bus_index < state.runtime_layout.event_input_bus_count;
            case AudioPluginGraphEndpointType::GraphOutput:
                return busType == AudioPluginGraphBusType::Audio
                    ? endpoint.bus_index < state.runtime_layout.audio_output_bus_count
                    : endpoint.bus_index < state.runtime_layout.event_output_bus_count;
            case AudioPluginGraphEndpointType::Plugin: {
                auto nodeId = endpointNodeId(endpoint);
                auto node = findNodeById(state, nodeId);
                if (!node)
                    return false;
                if (auto* pluginNode = dynamic_cast<AudioPluginNode*>(node.get())) {
                    auto* instance = pluginNode->instance();
                    if (!instance)
                        return false;
                    if (busType == AudioPluginGraphBusType::Audio) {
                        const bool sourceSide = endpoint.bus_index < pluginAudioOutputBusCount(instance);
                        const bool targetSide = endpoint.bus_index < pluginAudioInputBusCount(instance);
                        return sourceSide || targetSide;
                    }
                    const bool sourceSide = endpoint.bus_index < pluginEventOutputBusCount(instance);
                    const bool targetSide = endpoint.bus_index < pluginEventInputBusCount(instance);
                    return sourceSide || targetSide;
                }
                if (busType == AudioPluginGraphBusType::Audio) {
                    const auto outputCounts = node->requiredAudioOutputChannelCounts();
                    const auto inputCounts = node->requiredAudioInputChannelCounts();
                    const auto outputBusCount = !outputCounts.empty()
                        ? static_cast<uint32_t>(outputCounts.size())
                        : state.runtime_layout.audio_output_bus_count;
                    const auto inputBusCount = !inputCounts.empty()
                        ? static_cast<uint32_t>(inputCounts.size())
                        : state.runtime_layout.audio_input_bus_count;
                    const bool sourceSide = endpoint.bus_index < outputBusCount;
                    const bool targetSide = endpoint.bus_index < inputBusCount;
                    return sourceSide || targetSide;
                }
                const bool sourceSide = endpoint.bus_index < state.runtime_layout.event_output_bus_count;
                const bool targetSide = endpoint.bus_index < state.runtime_layout.event_input_bus_count;
                return sourceSide || targetSide;
            }
        }
        return false;
    }

    void AudioPluginFullDAGraphImpl::rebuildSimpleConnections(GraphState& state) const {
        state.connections.clear();
        state.output_audio_links.clear();
        state.output_event_links.clear();
        if (state.custom_topology)
            return;

        int64_t nextId = 1;
        auto orderedNodes = allNodes(state);
        if (orderedNodes.empty()) {
            state.next_connection_id = nextId;
            rebuildCompiledState(state);
            return;
        }

        auto connectAudioSpan = [&](const AudioPluginGraphEndpoint& source,
                                    uint32_t sourceBusCount,
                                    const AudioPluginGraphEndpoint& target,
                                    uint32_t targetBusCount) {
            const uint32_t count = std::min(sourceBusCount, targetBusCount);
            for (uint32_t bus = 0; bus < count; ++bus) {
                auto src = source;
                auto dst = target;
                src.bus_index = bus;
                dst.bus_index = bus;
                state.connections.push_back(AudioPluginGraphConnection{
                    nextId++,
                    AudioPluginGraphBusType::Audio,
                    src,
                    dst
                });
            }
        };

        auto connectEventSpan = [&](const AudioPluginGraphEndpoint& source,
                                    uint32_t sourceBusCount,
                                    const AudioPluginGraphEndpoint& target,
                                    uint32_t targetBusCount) {
            const uint32_t count = std::min(sourceBusCount, targetBusCount);
            for (uint32_t bus = 0; bus < count; ++bus) {
                auto src = source;
                auto dst = target;
                src.bus_index = bus;
                dst.bus_index = bus;
                state.connections.push_back(AudioPluginGraphConnection{
                    nextId++,
                    AudioPluginGraphBusType::Event,
                    src,
                    dst
                });
            }
        };

        auto first = orderedNodes.front();
        const int32_t firstInstanceId = dynamic_cast<AudioPluginNode*>(first.get()) ? dynamic_cast<AudioPluginNode*>(first.get())->instanceId() : -1;
        connectAudioSpan(
            AudioPluginGraphEndpoint{AudioPluginGraphEndpointType::GraphInput, {}, -1, 0},
            state.runtime_layout.audio_input_bus_count,
            AudioPluginGraphEndpoint{AudioPluginGraphEndpointType::Plugin, first->nodeId(), firstInstanceId, 0},
            nodeInputBusCount(state, first, AudioPluginGraphBusType::Audio));
        connectEventSpan(
            AudioPluginGraphEndpoint{AudioPluginGraphEndpointType::GraphInput, {}, -1, 0},
            state.runtime_layout.event_input_bus_count,
            AudioPluginGraphEndpoint{AudioPluginGraphEndpointType::Plugin, first->nodeId(), firstInstanceId, 0},
            nodeInputBusCount(state, first, AudioPluginGraphBusType::Event));

        for (const auto& node : orderedNodes) {
            const int32_t instanceId = dynamic_cast<AudioPluginNode*>(node.get()) ? dynamic_cast<AudioPluginNode*>(node.get())->instanceId() : -1;
            connectEventSpan(
                AudioPluginGraphEndpoint{AudioPluginGraphEndpointType::Plugin, node->nodeId(), instanceId, 0},
                nodeOutputBusCount(state, node, AudioPluginGraphBusType::Event),
                AudioPluginGraphEndpoint{AudioPluginGraphEndpointType::GraphOutput, {}, -1, 0},
                state.runtime_layout.event_output_bus_count);
        }

        for (size_t i = 0; i + 1 < orderedNodes.size(); ++i) {
            auto src = orderedNodes[i];
            auto dst = orderedNodes[i + 1];
            const int32_t srcInstanceId = dynamic_cast<AudioPluginNode*>(src.get()) ? dynamic_cast<AudioPluginNode*>(src.get())->instanceId() : -1;
            const int32_t dstInstanceId = dynamic_cast<AudioPluginNode*>(dst.get()) ? dynamic_cast<AudioPluginNode*>(dst.get())->instanceId() : -1;
            connectAudioSpan(
                AudioPluginGraphEndpoint{AudioPluginGraphEndpointType::Plugin, src->nodeId(), srcInstanceId, 0},
                nodeOutputBusCount(state, src, AudioPluginGraphBusType::Audio),
                AudioPluginGraphEndpoint{AudioPluginGraphEndpointType::Plugin, dst->nodeId(), dstInstanceId, 0},
                nodeInputBusCount(state, dst, AudioPluginGraphBusType::Audio));
        }

        auto last = orderedNodes.back();
        const int32_t lastInstanceId = dynamic_cast<AudioPluginNode*>(last.get()) ? dynamic_cast<AudioPluginNode*>(last.get())->instanceId() : -1;
        connectAudioSpan(
            AudioPluginGraphEndpoint{AudioPluginGraphEndpointType::Plugin, last->nodeId(), lastInstanceId, 0},
            nodeOutputBusCount(state, last, AudioPluginGraphBusType::Audio),
            AudioPluginGraphEndpoint{AudioPluginGraphEndpointType::GraphOutput, {}, -1, 0},
            state.runtime_layout.audio_output_bus_count);

        state.next_connection_id = nextId;
        rebuildCompiledState(state);
    }

    void AudioPluginFullDAGraphImpl::rebuildCompiledState(GraphState& state) const {
        state.runtimes.clear();
        state.topo_order.clear();
        state.output_audio_links.clear();
        state.output_event_links.clear();
        state.output_latencies.assign(state.runtime_layout.audio_output_bus_count, 0);
        state.output_tails.assign(state.runtime_layout.audio_output_bus_count, 0.0);
        state.has_cycle = false;

        std::unordered_map<std::string, std::vector<std::string>> adjacency;
        std::unordered_map<std::string, uint32_t> indegree;
        auto orderedNodes = allNodes(state);
        for (const auto& node : orderedNodes) {
            if (!node)
                continue;
            indegree[node->nodeId()] = 0;
            auto runtime = std::make_shared<GraphNodeRuntime>(node, event_buffer_size_in_bytes_);
            if (auto* pluginNode = dynamic_cast<AudioPluginNode*>(node.get())) {
                auto* instance = pluginNode->instance();
                auto* buses = instance->audioBuses();
                if (buses) {
                    runtime->process.configureAudioInputBuses(
                        buildAudioBusSpecs(buses->audioInputBuses(), kGraphScratchCapacityFrames));
                    runtime->process.configureAudioOutputBuses(
                        buildAudioBusSpecs(buses->audioOutputBuses(), kGraphScratchCapacityFrames));
                }
            } else {
                std::vector<remidy::AudioBusSpec> audioInputSpecs;
                std::vector<remidy::AudioBusSpec> audioOutputSpecs;

                auto requiredInputs = node->requiredAudioInputChannelCounts();
                if (!requiredInputs.empty()) {
                    audioInputSpecs.reserve(requiredInputs.size());
                    for (auto channels : requiredInputs)
                        audioInputSpecs.push_back(remidy::AudioBusSpec{remidy::AudioBusRole::Main, channels, kGraphScratchCapacityFrames});
                } else {
                    audioInputSpecs.reserve(state.runtime_layout.audio_input_bus_count);
                    for (uint32_t bus = 0; bus < state.runtime_layout.audio_input_bus_count; ++bus)
                        audioInputSpecs.push_back(remidy::AudioBusSpec{remidy::AudioBusRole::Main, kDefaultBuiltInChannelCount, kGraphScratchCapacityFrames});
                }

                auto requiredOutputs = node->requiredAudioOutputChannelCounts();
                if (!requiredOutputs.empty()) {
                    audioOutputSpecs.reserve(requiredOutputs.size());
                    for (auto channels : requiredOutputs)
                        audioOutputSpecs.push_back(remidy::AudioBusSpec{remidy::AudioBusRole::Main, channels, kGraphScratchCapacityFrames});
                } else {
                    audioOutputSpecs.reserve(state.runtime_layout.audio_output_bus_count);
                    for (uint32_t bus = 0; bus < state.runtime_layout.audio_output_bus_count; ++bus)
                        audioOutputSpecs.push_back(remidy::AudioBusSpec{remidy::AudioBusRole::Main, kDefaultBuiltInChannelCount, kGraphScratchCapacityFrames});
                }

                runtime->process.configureAudioInputBuses(audioInputSpecs);
                runtime->process.configureAudioOutputBuses(audioOutputSpecs);
            }
            state.runtimes[node->nodeId()] = std::move(runtime);
        }

        for (const auto& connection : state.connections) {
            if (!isValidEndpointDirection(connection))
                continue;
            if (!endpointExists(state, connection.source, connection.bus_type) ||
                !endpointExists(state, connection.target, connection.bus_type))
                continue;

            if (connection.target.type == AudioPluginGraphEndpointType::GraphOutput) {
                if (connection.bus_type == AudioPluginGraphBusType::Audio)
                    state.output_audio_links.push_back(connection);
                else
                    state.output_event_links.push_back(connection);
                if (!endpointNodeId(connection.source).empty())
                    adjacency[endpointNodeId(connection.source)];
                continue;
            }

            if (connection.target.type != AudioPluginGraphEndpointType::Plugin)
                continue;

            auto runtimeIt = state.runtimes.find(endpointNodeId(connection.target));
            if (runtimeIt == state.runtimes.end())
                continue;
            if (connection.bus_type == AudioPluginGraphBusType::Audio)
                runtimeIt->second->incoming_audio.push_back(connection);
            else
                runtimeIt->second->incoming_event.push_back(connection);

            if (!endpointNodeId(connection.source).empty()) {
                adjacency[endpointNodeId(connection.source)].push_back(endpointNodeId(connection.target));
                indegree[endpointNodeId(connection.target)] += 1;
            }
        }

        std::queue<std::string> ready;
        for (const auto& [nodeId, degree] : indegree)
            if (degree == 0)
                ready.push(nodeId);

        while (!ready.empty()) {
            const auto nodeId = ready.front();
            ready.pop();
            state.topo_order.push_back(nodeId);
            for (const auto& nextId : adjacency[nodeId]) {
                auto it = indegree.find(nextId);
                if (it == indegree.end() || it->second == 0)
                    continue;
                it->second -= 1;
                if (it->second == 0)
                    ready.push(nextId);
            }
        }

        if (state.topo_order.size() != state.runtimes.size()) {
            state.has_cycle = true;
            state.topo_order.clear();
            for (const auto& node : orderedNodes)
                if (node)
                    state.topo_order.push_back(node->nodeId());
        }

        std::unordered_map<std::string, uint32_t> nodeLatencies;
        std::unordered_map<std::string, double> nodeTails;
        for (const auto& nodeId : state.topo_order) {
            auto runtimeIt = state.runtimes.find(nodeId);
            if (runtimeIt == state.runtimes.end() || !runtimeIt->second->node)
                continue;
            uint32_t baseLatency = 0;
            double baseTail = 0.0;
            for (const auto& connection : runtimeIt->second->incoming_audio) {
                auto sourceNodeId = endpointNodeId(connection.source);
                if (sourceNodeId.empty())
                    continue;
                baseLatency = std::max(baseLatency, nodeLatencies[sourceNodeId]);
                const auto tail = nodeTails[sourceNodeId];
                if (std::isinf(tail))
                    baseTail = std::numeric_limits<double>::infinity();
                else if (!std::isinf(baseTail))
                    baseTail = std::max(baseTail, tail);
            }
            const auto latency = runtimeIt->second->node->latencyInSamples();
            const auto tail = runtimeIt->second->node->tailLengthInSeconds();
            nodeLatencies[nodeId] = baseLatency > std::numeric_limits<uint32_t>::max() - latency
                ? std::numeric_limits<uint32_t>::max()
                : baseLatency + latency;
            if (std::isinf(baseTail) || std::isinf(tail))
                nodeTails[nodeId] = std::numeric_limits<double>::infinity();
            else
                nodeTails[nodeId] = baseTail + std::max(0.0, tail);
        }

        for (const auto& connection : state.output_audio_links) {
            if (connection.target.bus_index >= state.output_latencies.size())
                continue;
            auto sourceNodeId = endpointNodeId(connection.source);
            if (sourceNodeId.empty())
                continue;
            state.output_latencies[connection.target.bus_index] = std::max(
                state.output_latencies[connection.target.bus_index],
                nodeLatencies[sourceNodeId]);
            const auto tail = nodeTails[sourceNodeId];
            if (std::isinf(tail))
                state.output_tails[connection.target.bus_index] = std::numeric_limits<double>::infinity();
            else if (!std::isinf(state.output_tails[connection.target.bus_index]))
                state.output_tails[connection.target.bus_index] = std::max(
                    state.output_tails[connection.target.bus_index],
                    tail);
        }
    }

    uint8_t AudioPluginFullDAGraphImpl::resolveGroup(int32_t instanceId) const {
        return group_resolver_ ? group_resolver_(instanceId) : static_cast<uint8_t>(0xFF);
    }

    uapmd_status_t AudioPluginFullDAGraphImpl::appendNodeSimple(int32_t instanceId,
                                                          AudioPluginInstanceAPI* instance,
                                                          std::function<void()>&& onDelete) {
        if (!instance)
            return -1;
        RTGraphState::ScopedAccess<farbot::ThreadType::nonRealtime> access(state_);
        access->nodes.push_back(std::make_shared<AudioPluginNodeImpl>(
            instanceId, instance, event_buffer_size_in_bytes_, std::move(onDelete)));
        if (access->custom_topology)
            rebuildCompiledState(*access);
        else
            rebuildSimpleConnections(*access);
        return 0;
    }

    uapmd_status_t AudioPluginFullDAGraphImpl::appendBuiltInNodeSimple(const AudioGraphNodeDescriptor& descriptor) {
        if (descriptor.node_type.empty() || !registry_)
            return -1;
        auto* factory = registry_->findBuiltInFactory(descriptor.node_type);
        if (!factory)
            return -1;
        auto node = factory->create(descriptor);
        if (!node)
            return -1;
        RTGraphState::ScopedAccess<farbot::ThreadType::nonRealtime> access(state_);
        for (const auto& existing : access->builtin_nodes)
            if (existing && existing->nodeId() == descriptor.node_id)
                return -1;
        access->builtin_nodes.push_back(std::move(node));
        if (access->custom_topology)
            rebuildCompiledState(*access);
        else
            rebuildSimpleConnections(*access);
        return 0;
    }

    bool AudioPluginFullDAGraphImpl::removeNodeSimple(int32_t instanceId) {
        std::shared_ptr<AudioPluginNodeImpl> removed;
        {
            RTGraphState::ScopedAccess<farbot::ThreadType::nonRealtime> access(state_);
            auto& nodes = access->nodes;
            for (auto it = nodes.begin(); it != nodes.end(); ++it) {
                if (*it && (*it)->instanceId() == instanceId) {
                    removed = std::move(*it);
                    nodes.erase(it);
                    break;
                }
            }
            if (!removed)
                return false;
            std::erase_if(access->connections, [instanceId](const AudioPluginGraphConnection& connection) {
                return (connection.source.type == AudioPluginGraphEndpointType::Plugin &&
                        connection.source.instance_id == instanceId) ||
                    (connection.target.type == AudioPluginGraphEndpointType::Plugin &&
                     connection.target.instance_id == instanceId);
            });
            if (access->custom_topology)
                rebuildCompiledState(*access);
            else
                rebuildSimpleConnections(*access);
        }
        return removed != nullptr;
    }

    std::map<int32_t, AudioPluginNode*> AudioPluginFullDAGraphImpl::plugins() {
        RTGraphState::ScopedAccess<farbot::ThreadType::nonRealtime> access(state_);
        std::map<int32_t, AudioPluginNode*> result;
        for (const auto& node : access->nodes)
            if (node)
                result[node->instanceId()] = node.get();
        return result;
    }

    std::map<std::string, AudioGraphNode*> AudioPluginFullDAGraphImpl::nodes() {
        RTGraphState::ScopedAccess<farbot::ThreadType::nonRealtime> access(state_);
        std::map<std::string, AudioGraphNode*> result;
        for (const auto& node : access->nodes)
            if (node)
                result[node->nodeId()] = node.get();
        for (const auto& node : access->builtin_nodes)
            if (node)
                result[node->nodeId()] = node.get();
        return result;
    }

    AudioGraphNode* AudioPluginFullDAGraphImpl::getNode(const std::string& nodeId) {
        RTGraphState::ScopedAccess<farbot::ThreadType::nonRealtime> access(state_);
        for (const auto& node : access->nodes)
            if (node && node->nodeId() == nodeId)
                return node.get();
        for (const auto& node : access->builtin_nodes)
            if (node && node->nodeId() == nodeId)
                return node.get();
        return nullptr;
    }

    AudioPluginNode* AudioPluginFullDAGraphImpl::getPluginNode(int32_t instanceId) {
        RTGraphState::ScopedAccess<farbot::ThreadType::nonRealtime> access(state_);
        auto node = findNode(*access, instanceId);
        return node ? node.get() : nullptr;
    }

    void AudioPluginFullDAGraphImpl::refreshTimingInfo() {
        RTGraphState::ScopedAccess<farbot::ThreadType::nonRealtime> access(state_);
        if (access->custom_topology)
            rebuildCompiledState(*access);
        else
            rebuildSimpleConnections(*access);
    }

    std::vector<AudioPluginGraphConnection> AudioPluginFullDAGraphImpl::connections() {
        RTGraphState::ScopedAccess<farbot::ThreadType::nonRealtime> access(state_);
        return access->connections;
    }

    void AudioPluginFullDAGraphImpl::saveTo(std::map<std::string, std::string>& entries) {
        entries.clear();
        if (!output_routing_rules_.empty())
            entries[kOutputRoutingPropertyKey] = serializeOutputRoutingRules(output_routing_rules_);
    }

    void AudioPluginFullDAGraphImpl::loadFrom(const std::map<std::string, std::string>& entries) {
        auto it = entries.find(kOutputRoutingPropertyKey);
        output_routing_rules_ = it != entries.end()
            ? parseOutputRoutingRules(it->second)
            : std::vector<TrackOutputRoutingRule>{};
    }

    std::vector<TrackOutputRoutingRule> AudioPluginFullDAGraphImpl::outputRoutingRules() const {
        return output_routing_rules_;
    }

    void AudioPluginFullDAGraphImpl::outputRoutingRules(const std::vector<TrackOutputRoutingRule>& rules) {
        output_routing_rules_ = rules;
    }

    TrackOutputRoutingTarget AudioPluginFullDAGraphImpl::outputRoutingTarget(uint32_t outputBusIndex) const {
        const auto it = std::find_if(output_routing_rules_.begin(), output_routing_rules_.end(),
            [outputBusIndex](const TrackOutputRoutingRule& rule) {
                return rule.source_bus_index == outputBusIndex;
            });
        return it != output_routing_rules_.end() ? it->target : TrackOutputRoutingTarget{};
    }

    uapmd_status_t AudioPluginFullDAGraphImpl::connect(const AudioPluginGraphConnection& connection) {
        RTGraphState::ScopedAccess<farbot::ThreadType::nonRealtime> access(state_);
        if (!isValidEndpointDirection(connection))
            return -1;
        if (!endpointExists(*access, connection.source, connection.bus_type) ||
            !endpointExists(*access, connection.target, connection.bus_type))
            return -1;
        if (connection.source.type == AudioPluginGraphEndpointType::GraphOutput ||
            connection.target.type == AudioPluginGraphEndpointType::GraphInput)
            return -1;

        AudioPluginGraphConnection stored = connection;
        stored.id = stored.id > 0 ? stored.id : access->next_connection_id++;
        access->custom_topology = true;
        access->connections.push_back(stored);

        rebuildCompiledState(*access);
        if (access->has_cycle) {
            access->connections.pop_back();
            rebuildCompiledState(*access);
            return -1;
        }
        return 0;
    }

    bool AudioPluginFullDAGraphImpl::disconnect(int64_t connectionId) {
        RTGraphState::ScopedAccess<farbot::ThreadType::nonRealtime> access(state_);
        auto oldSize = access->connections.size();
        std::erase_if(access->connections, [connectionId](const AudioPluginGraphConnection& connection) {
            return connection.id == connectionId;
        });
        if (oldSize == access->connections.size())
            return false;
        access->custom_topology = true;
        rebuildCompiledState(*access);
        return true;
    }

    void AudioPluginFullDAGraphImpl::clearConnections() {
        RTGraphState::ScopedAccess<farbot::ThreadType::nonRealtime> access(state_);
        access->custom_topology = true;
        access->connections.clear();
        rebuildCompiledState(*access);
    }

    std::vector<std::shared_ptr<AudioGraphNode>> AudioPluginFullDAGraphImpl::releaseNodesForMigration() {
        std::vector<NodePtr> releasedPlugins;
        std::vector<std::shared_ptr<AudioGraphNode>> releasedBuiltins;
        {
            RTGraphState::ScopedAccess<farbot::ThreadType::nonRealtime> access(state_);
            releasedPlugins = std::move(access->nodes);
            releasedBuiltins = std::move(access->builtin_nodes);
            access->nodes.clear();
            access->builtin_nodes.clear();
            access->runtimes.clear();
            access->topo_order.clear();
            access->connections.clear();
            access->output_audio_links.clear();
            access->output_event_links.clear();
            access->output_latencies.assign(access->runtime_layout.audio_output_bus_count, 0);
            access->output_tails.assign(access->runtime_layout.audio_output_bus_count, 0.0);
            access->custom_topology = false;
            access->has_cycle = false;
            access->next_connection_id = 1;
        }

        std::vector<std::shared_ptr<AudioGraphNode>> result;
        result.reserve(releasedPlugins.size() + releasedBuiltins.size());
        for (auto& node : releasedPlugins)
            if (node)
                result.push_back(std::move(node));
        for (auto& node : releasedBuiltins)
            if (node)
                result.push_back(std::move(node));
        return result;
    }

    bool AudioPluginFullDAGraphImpl::adoptNodesFromMigration(std::vector<std::shared_ptr<AudioGraphNode>>&& nodes) {
        RTGraphState::ScopedAccess<farbot::ThreadType::nonRealtime> access(state_);
        for (auto& transferred : nodes) {
            if (auto pluginNode = std::dynamic_pointer_cast<AudioPluginNodeImpl>(transferred)) {
                access->nodes.push_back(std::move(pluginNode));
            } else {
                // Built-in node (e.g. GainNode) — store in the separate list.
                access->builtin_nodes.push_back(std::move(transferred));
            }
        }
        if (access->custom_topology)
            rebuildCompiledState(*access);
        else
            rebuildSimpleConnections(*access);
        return true;
    }

    AudioGraphBusesLayout AudioPluginFullDAGraphImpl::busesLayout() {
        RTGraphState::ScopedAccess<farbot::ThreadType::nonRealtime> access(state_);
        return access->runtime_layout;
    }

    void AudioPluginFullDAGraphImpl::applyBusesLayout(const AudioGraphBusesLayout& layout) {
        RTGraphState::ScopedAccess<farbot::ThreadType::nonRealtime> access(state_);
        access->runtime_layout = AudioGraphBusesLayout{
            std::max(1u, layout.audio_input_bus_count),
            std::max(1u, layout.audio_output_bus_count),
            std::max(1u, layout.event_input_bus_count),
            std::max(1u, layout.event_output_bus_count),
        };
        if (access->custom_topology)
            rebuildCompiledState(*access);
        else
            rebuildSimpleConnections(*access);
    }

    void AudioPluginFullDAGraphImpl::setGroupResolver(std::function<uint8_t(int32_t)> resolver) {
        group_resolver_ = std::move(resolver);
    }

    void AudioPluginFullDAGraphImpl::setEventOutputCallback(std::function<void(int32_t, const uapmd_ump_t*, size_t)> callback) {
        event_output_callback_ = std::move(callback);
    }

    int32_t AudioPluginFullDAGraphImpl::processAudio(AudioProcessContext& process) {
        RTGraphState::ScopedAccess<farbot::ThreadType::realtime> access(state_);
        auto& state = *access;

        if (state.nodes.empty() && state.builtin_nodes.empty()) {
            process.copyInputsToOutputs();
            return 0;
        }

        process.clearAudioOutputs();

        for (const auto& nodeId : state.topo_order) {
            auto runtimeIt = state.runtimes.find(nodeId);
            if (runtimeIt == state.runtimes.end())
                continue;
            auto& runtime = *runtimeIt->second;
            if (!runtime.node)
                continue;
            auto* pluginImpl = dynamic_cast<AudioPluginNodeImpl*>(runtime.node.get());
            auto* pluginNode = dynamic_cast<AudioPluginNode*>(runtime.node.get());
            auto* instance = pluginNode ? pluginNode->instance() : nullptr;

            syncMasterContext(runtime.master_context, process.masterContext());
            runtime.process.frameCount(process.frameCount());
            clearAudioInputs(runtime.process);
            runtime.process.clearAudioOutputs();

            for (const auto& connection : runtime.incoming_audio) {
                if (connection.source.type == AudioPluginGraphEndpointType::GraphInput) {
                    copyInputToInput(runtime.process, connection.target.bus_index, process, connection.source.bus_index);
                    continue;
                }
                auto sourceRuntimeIt = state.runtimes.find(endpointNodeId(connection.source));
                if (sourceRuntimeIt == state.runtimes.end())
                    continue;
                accumulateAudioBus(runtime.process, true, connection.target.bus_index,
                                   sourceRuntimeIt->second->process, false, connection.source.bus_index);
            }

            uint8_t group = 0xFF;
            if (pluginImpl && pluginNode) {
                pluginImpl->drainQueueToPending();
                group = resolveGroup(pluginNode->instanceId());
                pluginImpl->fillEventBufferForGroup(runtime.process.eventIn(), group);
            }

            for (const auto& connection : runtime.incoming_event) {
                if (connection.source.type == AudioPluginGraphEndpointType::GraphInput) {
                    appendEventsForGroup(runtime.process.eventIn(), process.eventIn(), group);
                    continue;
                }
                auto sourceRuntimeIt = state.runtimes.find(endpointNodeId(connection.source));
                if (sourceRuntimeIt == state.runtimes.end())
                    continue;
                appendEventBytes(
                    runtime.process.eventIn(),
                    static_cast<const uint8_t*>(sourceRuntimeIt->second->process.eventOut().getMessages()),
                    sourceRuntimeIt->second->process.eventOut().position());
            }

            if (runtime.incoming_event.empty()) {
                for (const auto& connection : runtime.incoming_audio) {
                    auto sourceNodeId = endpointNodeId(connection.source);
                    if (sourceNodeId.empty())
                        continue;
                    auto sourceRuntimeIt = state.runtimes.find(sourceNodeId);
                    if (sourceRuntimeIt == state.runtimes.end())
                        continue;
                    appendEventBytes(
                        runtime.process.eventIn(),
                        static_cast<const uint8_t*>(sourceRuntimeIt->second->process.eventIn().getMessages()),
                        sourceRuntimeIt->second->process.eventIn().position());
                }
            }

            if (pluginImpl && pluginNode && instance) {
                pluginImpl->drainPresetRequests();
                if (!instance->bypassed())
                    pluginImpl->processInputMapping(runtime.process);

                if (instance->bypassed()) {
                    runtime.process.copyInputsToOutputs();
                } else {
                    auto status = instance->processAudio(runtime.process);
                    if (status != 0)
                        return status;
                }

                if (!state.custom_topology && event_output_callback_ && runtime.process.eventOut().position() > 0) {
                    event_output_callback_(
                        pluginNode->instanceId(),
                        static_cast<uapmd_ump_t*>(runtime.process.eventOut().getMessages()),
                        runtime.process.eventOut().position());
                }
            } else {
                auto status = runtime.node->processAudio(runtime.process);
                if (status != 0)
                    return status;
            }
        }

        for (const auto& connection : state.output_audio_links) {
            if (connection.source.type == AudioPluginGraphEndpointType::GraphInput) {
                copyInputToOutput(process, connection.target.bus_index, process, connection.source.bus_index);
                continue;
            }
            auto sourceRuntimeIt = state.runtimes.find(endpointNodeId(connection.source));
            if (sourceRuntimeIt == state.runtimes.end())
                continue;
            accumulateAudioBus(process, false, connection.target.bus_index,
                               sourceRuntimeIt->second->process, false, connection.source.bus_index);
        }

        if (state.custom_topology && event_output_callback_) {
            for (const auto& connection : state.output_event_links) {
                if (connection.source.type == AudioPluginGraphEndpointType::GraphInput) {
                    event_output_callback_(-1,
                                           static_cast<uapmd_ump_t*>(process.eventIn().getMessages()),
                                           process.eventIn().position());
                    continue;
                }
                auto sourceRuntimeIt = state.runtimes.find(endpointNodeId(connection.source));
                if (sourceRuntimeIt == state.runtimes.end())
                    continue;
                auto& eventOut = sourceRuntimeIt->second->process.eventOut();
                if (eventOut.position() == 0)
                    continue;
                if (auto* sourcePluginNode = dynamic_cast<AudioPluginNode*>(sourceRuntimeIt->second->node.get())) {
                    event_output_callback_(sourcePluginNode->instanceId(),
                                           static_cast<uapmd_ump_t*>(eventOut.getMessages()),
                                           eventOut.position());
                }
            }
        }

        return 0;
    }

    uint32_t AudioPluginFullDAGraphImpl::outputBusCount() {
        RTGraphState::ScopedAccess<farbot::ThreadType::nonRealtime> access(state_);
        return access->runtime_layout.audio_output_bus_count;
    }

    uint32_t AudioPluginFullDAGraphImpl::outputLatencyInSamples(uint32_t outputBusIndex) {
        RTGraphState::ScopedAccess<farbot::ThreadType::nonRealtime> access(state_);
        return outputBusIndex < access->output_latencies.size() ? access->output_latencies[outputBusIndex] : 0;
    }

    double AudioPluginFullDAGraphImpl::outputTailLengthInSeconds(uint32_t outputBusIndex) {
        RTGraphState::ScopedAccess<farbot::ThreadType::nonRealtime> access(state_);
        return outputBusIndex < access->output_tails.size() ? access->output_tails[outputBusIndex] : 0.0;
    }

    uint32_t AudioPluginFullDAGraphImpl::renderLeadInSamples() {
        uint32_t maxLatency = 0;
        const auto count = outputBusCount();
        for (uint32_t outputBusIndex = 0; outputBusIndex < count; ++outputBusIndex)
            maxLatency = std::max(maxLatency, outputLatencyInSamples(outputBusIndex));
        return maxLatency;
    }

    uint32_t AudioPluginFullDAGraphImpl::mainOutputLatencyInSamples() {
        return outputLatencyInSamples(0);
    }

    double AudioPluginFullDAGraphImpl::mainOutputTailLengthInSeconds() {
        return outputTailLengthInSeconds(0);
    }

    std::unique_ptr<AudioPluginFullDAGraph> AudioPluginFullDAGraph::create(size_t eventBufferSizeInBytes) {
        return std::make_unique<AudioPluginFullDAGraphImpl>(eventBufferSizeInBytes, std::string{});
    }

}
