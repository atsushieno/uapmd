#include "uapmd-data/uapmd-data.hpp"
#include "UapmdAudioPluginFullDAGraphData.hpp"
#include "UapmdProjectPluginListGraphData.hpp"

#include <choc/text/choc_JSON.h>
#include <fstream>
#include <optional>
#include <sstream>

namespace uapmd {

namespace {

const char* graphEndpointTypeToString(AudioPluginGraphEndpointType type) {
    switch (type) {
        case AudioPluginGraphEndpointType::GraphInput:
            return "graph_input";
        case AudioPluginGraphEndpointType::GraphOutput:
            return "graph_output";
        case AudioPluginGraphEndpointType::Plugin:
        default:
            return "plugin";
    }
}

AudioPluginGraphEndpointType parseGraphEndpointType(std::string_view value) {
    if (value == "graph_input")
        return AudioPluginGraphEndpointType::GraphInput;
    if (value == "graph_output")
        return AudioPluginGraphEndpointType::GraphOutput;
    return AudioPluginGraphEndpointType::Plugin;
}

const char* graphBusTypeToString(AudioPluginGraphBusType type) {
    return type == AudioPluginGraphBusType::Event ? "event" : "audio";
}

AudioPluginGraphBusType parseGraphBusType(std::string_view value) {
    return value == "event" ? AudioPluginGraphBusType::Event : AudioPluginGraphBusType::Audio;
}

std::optional<choc::value::Value> parseGraphJsonObject(const std::vector<uint8_t>& bytes) {
    if (bytes.empty())
        return std::nullopt;

    auto root = choc::json::parse(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    if (!root.isObject())
        return std::nullopt;
    return root;
}

void writeGraphJsonObject(const choc::value::Value& object, std::vector<uint8_t>& bytes) {
    auto json = choc::json::toString(object, true);
    bytes.assign(json.begin(), json.end());
}

template <typename GraphT>
void parsePluginListPayload(const choc::value::ValueView& root, GraphT& graph) {
    graph.clearPlugins();

    if (root.hasObjectMember("graph_type"))
        graph.graphType(std::string(root["graph_type"].getString()));

    if (root.hasObjectMember("plugins") && root["plugins"].isArray()) {
        for (const auto& pluginObj : root["plugins"]) {
            UapmdProjectPluginNodeData node;
            if (pluginObj.hasObjectMember("plugin_id"))
                node.plugin_id = std::string(pluginObj["plugin_id"].getString());
            if (pluginObj.hasObjectMember("format"))
                node.format = std::string(pluginObj["format"].getString());
            if (pluginObj.hasObjectMember("display_name"))
                node.display_name = std::string(pluginObj["display_name"].getString());
            if (pluginObj.hasObjectMember("state_file"))
                node.state_file = std::string(pluginObj["state_file"].getString());
            if (pluginObj.hasObjectMember("group_index"))
                node.group_index = pluginObj["group_index"].getWithDefault<int32_t>(-1);
            graph.addPlugin(std::move(node));
        }
    }
}

template <typename GraphT>
choc::value::Value serializePluginListPayload(GraphT& graph) {
    auto obj = choc::value::createObject("UapmdPluginGraph");
    obj.addMember("graph_type", graph.graphType());

    auto plugins = graph.plugins();
    if (!plugins.empty()) {
        auto pluginsArray = choc::value::createEmptyArray();
        for (const auto& plugin : plugins) {
            auto pluginObj = choc::value::createObject("Plugin");
            pluginObj.addMember("plugin_id", plugin.plugin_id);
            pluginObj.addMember("format", plugin.format);
            pluginObj.addMember("display_name", plugin.display_name);
            pluginObj.addMember("state_file", plugin.state_file);
            pluginObj.addMember("group_index", static_cast<int64_t>(plugin.group_index));
            pluginsArray.addArrayElement(pluginObj);
        }
        obj.addMember("plugins", pluginsArray);
    }

    return obj;
}

bool loadPluginListGraphJsonFile(UapmdProjectPluginListGraphData* graph, const std::vector<uint8_t>& bytes) {
    if (!graph)
        return false;
    auto root = parseGraphJsonObject(bytes);
    if (!root)
        return false;
    parsePluginListPayload(*root, *graph);
    return true;
}

bool savePluginListGraphJsonFile(UapmdProjectPluginListGraphData* graph, std::vector<uint8_t>& bytes) {
    if (!graph)
        return false;
    writeGraphJsonObject(serializePluginListPayload(*graph), bytes);
    return true;
}

bool loadFullDAGGraphJsonFile(UapmdAudioPluginFullDAGraphData* graph, const std::vector<uint8_t>& bytes) {
    if (!graph)
        return false;
    auto root = parseGraphJsonObject(bytes);
    if (!root)
        return false;

    parsePluginListPayload(*root, *graph);
    graph->clearConnections();

    if (root->hasObjectMember("connections") && (*root)["connections"].isArray()) {
        for (const auto& connectionObj : (*root)["connections"]) {
            UapmdProjectPluginGraphConnectionData connection;
            if (connectionObj.hasObjectMember("id"))
                connection.id = connectionObj["id"].getWithDefault<int64_t>(0);
            if (connectionObj.hasObjectMember("bus_type"))
                connection.bus_type = parseGraphBusType(connectionObj["bus_type"].getString());
            if (connectionObj.hasObjectMember("source") && connectionObj["source"].isObject()) {
                auto endpointObj = connectionObj["source"];
                if (endpointObj.hasObjectMember("type"))
                    connection.source.type = parseGraphEndpointType(endpointObj["type"].getString());
                if (endpointObj.hasObjectMember("plugin_index"))
                    connection.source.plugin_index = endpointObj["plugin_index"].getWithDefault<int32_t>(-1);
                if (endpointObj.hasObjectMember("bus_index"))
                    connection.source.bus_index = endpointObj["bus_index"].getWithDefault<uint32_t>(0);
            }
            if (connectionObj.hasObjectMember("target") && connectionObj["target"].isObject()) {
                auto endpointObj = connectionObj["target"];
                if (endpointObj.hasObjectMember("type"))
                    connection.target.type = parseGraphEndpointType(endpointObj["type"].getString());
                if (endpointObj.hasObjectMember("plugin_index"))
                    connection.target.plugin_index = endpointObj["plugin_index"].getWithDefault<int32_t>(-1);
                if (endpointObj.hasObjectMember("bus_index"))
                    connection.target.bus_index = endpointObj["bus_index"].getWithDefault<uint32_t>(0);
            }
            graph->addConnection(std::move(connection));
        }
    }

    return true;
}

bool saveFullDAGGraphJsonFile(UapmdAudioPluginFullDAGraphData* graph, std::vector<uint8_t>& bytes) {
    if (!graph)
        return false;

    auto obj = serializePluginListPayload(*graph);
    auto connections = graph->connections();
    if (!connections.empty()) {
        auto connectionArray = choc::value::createEmptyArray();
        for (const auto& connection : connections) {
            auto connectionObj = choc::value::createObject("Connection");
            connectionObj.addMember("id", connection.id);
            connectionObj.addMember("bus_type", graphBusTypeToString(connection.bus_type));

            auto sourceObj = choc::value::createObject("Endpoint");
            sourceObj.addMember("type", graphEndpointTypeToString(connection.source.type));
            sourceObj.addMember("bus_index", static_cast<int64_t>(connection.source.bus_index));
            if (connection.source.type == AudioPluginGraphEndpointType::Plugin)
                sourceObj.addMember("plugin_index", static_cast<int64_t>(connection.source.plugin_index));
            connectionObj.addMember("source", sourceObj);

            auto targetObj = choc::value::createObject("Endpoint");
            targetObj.addMember("type", graphEndpointTypeToString(connection.target.type));
            targetObj.addMember("bus_index", static_cast<int64_t>(connection.target.bus_index));
            if (connection.target.type == AudioPluginGraphEndpointType::Plugin)
                targetObj.addMember("plugin_index", static_cast<int64_t>(connection.target.plugin_index));
            connectionObj.addMember("target", targetObj);

            connectionArray.addArrayElement(connectionObj);
        }
        obj.addMember("connections", connectionArray);
    }

    writeGraphJsonObject(obj, bytes);
    return true;
}

class SimpleLinearAudioGraphProvider final : public AudioGraphProvider {
public:
    const std::string& id() const override { return id_; }
    const std::string& label() const override { return label_; }

    std::unique_ptr<AudioPluginGraph> createGraph(size_t eventBufferSizeInBytes) const override {
        return AudioPluginGraph::create(eventBufferSizeInBytes);
    }

    bool deserializeRuntimeGraph(
        UapmdProjectPluginGraphData* data,
        AudioPluginGraph& graph,
        const std::vector<int32_t>& orderedInstanceIds) const override {
        return UapmdPluginGraphBuilder::build(data, graph, orderedInstanceIds);
    }

    bool loadProjectGraph(
        UapmdProjectPluginGraphData* data,
        const std::vector<uint8_t>& bytes) const override {
        return loadPluginListGraphJsonFile(dynamic_cast<UapmdProjectPluginListGraphData*>(data), bytes);
    }

    std::vector<UapmdProjectPluginNodeData> getPluginNodeDataListFrom(
        UapmdProjectPluginGraphData* data) const override {
        auto* pluginListGraph = dynamic_cast<UapmdProjectPluginListGraphData*>(data);
        return pluginListGraph ? pluginListGraph->plugins() : std::vector<UapmdProjectPluginNodeData>{};
    }

    void serializeRuntimeGraph(
        UapmdProjectPluginGraphData* graphData,
        AudioPluginGraph& runtimeGraph,
        const std::unordered_map<int32_t, int32_t>& instanceToIndex) const override {
        auto* pluginListGraph = dynamic_cast<UapmdProjectPluginListGraphData*>(graphData);
        if (!pluginListGraph)
            return;
        (void) runtimeGraph;
        (void) instanceToIndex;
    }

    bool saveProjectGraph(
        UapmdProjectPluginGraphData* graphData,
        std::vector<uint8_t>& bytes) const override {
        return savePluginListGraphJsonFile(dynamic_cast<UapmdProjectPluginListGraphData*>(graphData), bytes);
    }

private:
    std::string id_{};
    std::string label_{"Simple Linear"};
};

class FullDAGAudioGraphProvider final : public AudioGraphProvider {
public:
    const std::string& id() const override { return id_; }
    const std::string& label() const override { return label_; }

    std::unique_ptr<AudioPluginGraph> createGraph(size_t eventBufferSizeInBytes) const override {
        return std::unique_ptr<AudioPluginGraph>(AudioPluginFullDAGraph::create(eventBufferSizeInBytes).release());
    }

    bool deserializeRuntimeGraph(
        UapmdProjectPluginGraphData* data,
        AudioPluginGraph& graph,
        const std::vector<int32_t>& orderedInstanceIds) const override {
        return UapmdAudioPluginFullDAGraphBuilder::build(data, graph, orderedInstanceIds);
    }

    bool loadProjectGraph(
        UapmdProjectPluginGraphData* data,
        const std::vector<uint8_t>& bytes) const override {
        return loadFullDAGGraphJsonFile(dynamic_cast<UapmdAudioPluginFullDAGraphData*>(data), bytes);
    }

    std::vector<UapmdProjectPluginNodeData> getPluginNodeDataListFrom(
        UapmdProjectPluginGraphData* data) const override {
        auto* dagData = dynamic_cast<UapmdAudioPluginFullDAGraphData*>(data);
        return dagData ? dagData->plugins() : std::vector<UapmdProjectPluginNodeData>{};
    }

    void serializeRuntimeGraph(
        UapmdProjectPluginGraphData* graphData,
        AudioPluginGraph& runtimeGraph,
        const std::unordered_map<int32_t, int32_t>& instanceToIndex) const override {
        auto* dagData = dynamic_cast<UapmdAudioPluginFullDAGraphData*>(graphData);
        if (!dagData)
            return;

        auto* fullGraph = dynamic_cast<AudioPluginFullDAGraph*>(&runtimeGraph);
        if (!fullGraph)
            return;

        dagData->clearConnections();

        for (const auto& connection : fullGraph->connections()) {
            auto toProjectEndpoint = [&](const AudioPluginGraphEndpoint& endpoint)
                -> std::optional<UapmdProjectPluginGraphEndpointData> {
                UapmdProjectPluginGraphEndpointData result;
                result.type = endpoint.type;
                result.bus_index = endpoint.bus_index;
                if (endpoint.type == AudioPluginGraphEndpointType::Plugin) {
                    auto it = instanceToIndex.find(endpoint.instance_id);
                    if (it == instanceToIndex.end())
                        return std::nullopt;
                    result.plugin_index = it->second;
                }
                return result;
            };

            auto source = toProjectEndpoint(connection.source);
            auto target = toProjectEndpoint(connection.target);
            if (!source || !target)
                continue;

            dagData->addConnection(UapmdProjectPluginGraphConnectionData{
                .id = connection.id,
                .bus_type = connection.bus_type,
                .source = *source,
                .target = *target,
            });
        }
    }

    bool saveProjectGraph(
        UapmdProjectPluginGraphData* graphData,
        std::vector<uint8_t>& bytes) const override {
        return saveFullDAGGraphJsonFile(dynamic_cast<UapmdAudioPluginFullDAGraphData*>(graphData), bytes);
    }

private:
    std::string id_{"urn:uapmd-graph:common/graph/dag/v1"};
    std::string label_{"Full DAG"};
};

template <typename GraphDataType>
void populateProjectGraphPlugins(
    GraphDataType& graphData,
    const std::vector<int32_t>& orderedInstanceIds,
    AudioPluginGraph& runtimeGraph,
    const std::function<AudioPluginInstanceAPI*(int32_t instanceId)>& resolveInstance,
    const AudioGraphProvider::PluginStateFileCallback& pluginStateFileCallback,
    std::unordered_map<int32_t, int32_t>& instanceToIndex)
{
    graphData.clearPlugins();

    size_t pluginIndex = 0;
    for (int32_t instanceId : orderedInstanceIds) {
        if (instanceId < 0)
            continue;

        AudioPluginInstanceAPI* instance = nullptr;
        if (auto* node = runtimeGraph.getPluginNode(instanceId))
            instance = node->instance();
        if (!instance && resolveInstance)
            instance = resolveInstance(instanceId);
        if (!instance)
            continue;

        UapmdProjectPluginNodeData nodeData;
        nodeData.plugin_id = instance->pluginId();
        nodeData.format = instance->formatName();
        nodeData.display_name = instance->displayName();
        graphData.addPlugin(std::move(nodeData));

        auto nodeIndex = graphData.plugins().size() - 1;
        instanceToIndex[instanceId] = static_cast<int32_t>(nodeIndex);
        if (pluginStateFileCallback) {
            pluginStateFileCallback(instanceId, pluginIndex, instance,
                [&graphData, nodeIndex](const std::string& relativePath) {
                    auto nodes = graphData.plugins();
                    if (nodeIndex >= nodes.size())
                        return;
                    nodes[nodeIndex].state_file = relativePath;
                    graphData.setPlugins(std::move(nodes));
                });
        }
        ++pluginIndex;
    }
}

std::unique_ptr<UapmdProjectPluginGraphData> createProjectGraphDataForProvider(
    const AudioGraphProvider& provider)
{
    if (dynamic_cast<const FullDAGAudioGraphProvider*>(&provider)) {
        auto graphData = UapmdAudioPluginFullDAGraphData::create();
        graphData->graphType(provider.id());
        return graphData;
    }

    auto graphData = UapmdProjectPluginListGraphData::create();
    graphData->graphType(provider.id());
    return graphData;
}

} // namespace

std::unique_ptr<UapmdProjectPluginGraphData> createSerializedProjectGraph(
    const AudioGraphProvider& provider,
    const std::vector<int32_t>& orderedInstanceIds,
    AudioPluginGraph& runtimeGraph,
    const std::function<AudioPluginInstanceAPI*(int32_t instanceId)>& resolveInstance,
    const AudioGraphProvider::PluginStateFileCallback& pluginStateFileCallback)
{
    auto graphData = createProjectGraphDataForProvider(provider);
    if (!graphData)
        return {};

    std::unordered_map<int32_t, int32_t> instanceToIndex;
    if (auto* pluginListGraph = dynamic_cast<UapmdProjectPluginListGraphData*>(graphData.get())) {
        populateProjectGraphPlugins(
            *pluginListGraph,
            orderedInstanceIds,
            runtimeGraph,
            resolveInstance,
            pluginStateFileCallback,
            instanceToIndex);
    } else if (auto* dagGraph = dynamic_cast<UapmdAudioPluginFullDAGraphData*>(graphData.get())) {
        dagGraph->clearConnections();
        populateProjectGraphPlugins(
            *dagGraph,
            orderedInstanceIds,
            runtimeGraph,
            resolveInstance,
            pluginStateFileCallback,
            instanceToIndex);
    }

    provider.serializeRuntimeGraph(graphData.get(), runtimeGraph, instanceToIndex);
    return graphData;
}

std::unique_ptr<UapmdProjectPluginGraphData> loadSerializedProjectGraph(
    const AudioGraphProvider& provider,
    UapmdProjectPluginGraphData& metadata,
    const std::vector<uint8_t>& bytes)
{
    auto graphData = createProjectGraphDataForProvider(provider);
    if (!graphData)
        return {};

    graphData->graphType(metadata.graphType());
    graphData->externalFile(metadata.externalFile());
    if (!provider.loadProjectGraph(graphData.get(), bytes))
        return {};
    return graphData;
}

AudioGraphProviderRegistry AudioGraphProviderRegistry::create() {
    AudioGraphProviderRegistry registry;
    registry.add(std::make_unique<FullDAGAudioGraphProvider>());
    registry.add(std::make_unique<SimpleLinearAudioGraphProvider>());
    return registry;
}

AudioGraphProvider* AudioGraphProviderRegistry::add(std::unique_ptr<AudioGraphProvider> provider) {
    if (!provider)
        return nullptr;
    auto* raw = provider.get();
    providers_.push_back(std::move(provider));
    return raw;
}

bool AudioGraphProviderRegistry::remove(AudioGraphProvider* provider) {
    if (!provider)
        return false;
    auto before = providers_.size();
    std::erase_if(providers_, [&](const auto& item) {
        return item.get() == provider;
    });
    return providers_.size() != before;
}

void AudioGraphProviderRegistry::clear() {
    providers_.clear();
}

const AudioGraphProvider* AudioGraphProviderRegistry::get(const std::string& graphTypeId) const {
    auto it = std::ranges::find_if(providers_, [&](const auto& provider) {
        return provider && provider->id() == graphTypeId;
    });
    if (it != providers_.end())
        return it->get();

    auto fallback = std::ranges::find_if(providers_, [&](const auto& provider) {
        return provider && provider->id().empty();
    });
    return fallback != providers_.end() ? fallback->get() : nullptr;
}

const AudioGraphProvider* AudioGraphProviderRegistry::get(const AudioPluginGraph& graph) const {
    if (!graph.providerId().empty())
        return get(graph.providerId());

    if (dynamic_cast<const AudioPluginFullDAGraph*>(&graph) != nullptr) {
        auto it = std::ranges::find_if(providers_, [](const auto& provider) {
            return provider && !provider->id().empty();
        });
        if (it != providers_.end())
            return it->get();
    }

    return get(graph.providerId());
}

std::unique_ptr<AudioPluginGraph> AudioGraphProviderRegistry::createGraph(
    const std::string& graphTypeId,
    size_t eventBufferSizeInBytes) const {
    auto* provider = get(graphTypeId);
    return provider ? provider->createGraph(eventBufferSizeInBytes) : nullptr;
}

} // namespace uapmd
