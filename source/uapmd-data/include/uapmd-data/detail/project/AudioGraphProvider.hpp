#pragma once

#include <memory>
#include <string>
#include <functional>
#include <filesystem>
#include <unordered_map>
#include <vector>

#include <uapmd-graph/uapmd-graph.hpp>
#include "UapmdProjectFile.hpp"

namespace uapmd {

class AudioGraphProvider {
public:
    virtual ~AudioGraphProvider() = default;

    using PluginStateFileCallback = std::function<void(
        int32_t instanceId,
        size_t pluginOrder,
        uapmd_plugin_hosting::AudioPluginInstanceAPI* instance,
        const std::function<void(const std::string& relativePath)>& setStateFile)>;
    virtual const std::string& id() const = 0;
    virtual const std::string& label() const = 0;
    // Whether graphs from this provider offer
    // `uapmd_graph::GraphConnectionExtension`. Callers use this to pick a
    // provider before creating a graph, instead of creating one to find out.
    virtual bool supportsConnectionEditing() const = 0;
    // FIXME: we should remove the argument and use some global value instead.
    // Having separate sizes for every single graph here makes things unstable.
    virtual std::unique_ptr<uapmd_graph::AudioPluginGraph> createGraph(size_t eventBufferSizeInBytes) const = 0;
    virtual bool deserializeRuntimeGraph(
        UapmdProjectPluginGraphData* data,
        uapmd_graph::AudioPluginGraph& graph,
        const std::vector<int32_t>& orderedInstanceIds) const = 0;
    virtual bool loadProjectGraph(
        UapmdProjectPluginGraphData* data,
        const std::vector<uint8_t>& bytes) const = 0;
    virtual std::vector<UapmdProjectPluginNodeData> getPluginNodeDataListFrom(
        UapmdProjectPluginGraphData* data) const = 0;
    virtual void serializeRuntimeGraph(
        UapmdProjectPluginGraphData* graphData,
        uapmd_graph::AudioPluginGraph& runtimeGraph,
        const std::unordered_map<int32_t, int32_t>& instanceToIndex) const = 0;
    virtual bool saveProjectGraph(
        UapmdProjectPluginGraphData* graphData,
        std::vector<uint8_t>& bytes) const = 0;
};

std::unique_ptr<UapmdProjectPluginGraphData> createSerializedProjectGraph(
    const AudioGraphProvider& provider,
    const std::vector<int32_t>& orderedInstanceIds,
    uapmd_graph::AudioPluginGraph& runtimeGraph,
    const std::function<uapmd_plugin_hosting::AudioPluginInstanceAPI*(int32_t instanceId)>& resolveInstance,
    const AudioGraphProvider::PluginStateFileCallback& pluginStateFileCallback = {});
std::unique_ptr<UapmdProjectPluginGraphData> loadSerializedProjectGraph(
    const AudioGraphProvider& provider,
    UapmdProjectPluginGraphData& metadata,
    const std::vector<uint8_t>& bytes);

// The provider that turned out to be able to load a graph, and what it made
// of it. `provider` is null when no provider could load the bytes at all.
struct LoadedProjectGraph {
    const AudioGraphProvider* provider{};
    std::unique_ptr<UapmdProjectPluginGraphData> data{};
};

class AudioGraphProviderRegistry {
public:
    static AudioGraphProviderRegistry create();

    AudioGraphProvider* add(std::unique_ptr<AudioGraphProvider> provider);
    bool remove(AudioGraphProvider* provider);
    void clear();
    const AudioGraphProvider* get(const std::string& graphTypeId) const;
    const AudioGraphProvider* get(const uapmd_graph::AudioPluginGraph& graph) const;
    // The first registered provider whose graphs support connection editing,
    // or nullptr when none does.
    const AudioGraphProvider* findConnectionEditingProvider() const;
    // Finds a provider that can load `bytes`, by asking them to. The graph
    // type recorded in `metadata` is a preference, not a gate: a graph written
    // by one provider is often loadable by another -- a linear chain is a
    // degenerate DAG -- and the provider that wrote it may not be present in
    // this build at all.
    //
    // This relies on providers reporting failure from their loaders when they
    // cannot represent the input. A provider that accepts a graph and quietly
    // drops what it cannot express would defeat it.
    LoadedProjectGraph loadWithAnyProvider(
        UapmdProjectPluginGraphData& metadata,
        const std::vector<uint8_t>& bytes) const;
    std::unique_ptr<uapmd_graph::AudioPluginGraph> createGraph(
        const std::string& graphTypeId,
        size_t eventBufferSizeInBytes) const;

private:
    std::vector<std::unique_ptr<AudioGraphProvider>> providers_{};
};

} // namespace uapmd
