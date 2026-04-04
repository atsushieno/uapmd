#pragma once

#include <memory>
#include <string>
#include <functional>
#include <filesystem>
#include <unordered_map>
#include <vector>

#include <uapmd/uapmd.hpp>
#include "UapmdProjectFile.hpp"

namespace uapmd {

class AudioGraphProvider {
public:
    virtual ~AudioGraphProvider() = default;

    using PluginStateFileCallback = std::function<void(
        int32_t instanceId,
        size_t pluginOrder,
        AudioPluginInstanceAPI* instance,
        const std::function<void(const std::string& relativePath)>& setStateFile)>;
    virtual const std::string& id() const = 0;
    virtual const std::string& label() const = 0;
    // FIXME: we should remove the argument and use some global value instead.
    // Having separate sizes for every single graph here makes things unstable.
    virtual std::unique_ptr<AudioPluginGraph> createGraph(size_t eventBufferSizeInBytes) const = 0;
    virtual bool deserializeRuntimeGraph(
        UapmdProjectPluginGraphData* data,
        AudioPluginGraph& graph,
        const std::vector<int32_t>& orderedInstanceIds) const = 0;
    virtual bool loadProjectGraph(
        UapmdProjectPluginGraphData* data,
        const std::vector<uint8_t>& bytes) const = 0;
    virtual std::vector<UapmdProjectPluginNodeData> getPluginNodeDataListFrom(
        UapmdProjectPluginGraphData* data) const = 0;
    virtual void serializeRuntimeGraph(
        UapmdProjectPluginGraphData* graphData,
        AudioPluginGraph& runtimeGraph,
        const std::unordered_map<int32_t, int32_t>& instanceToIndex) const = 0;
    virtual bool saveProjectGraph(
        UapmdProjectPluginGraphData* graphData,
        std::vector<uint8_t>& bytes) const = 0;
};

std::unique_ptr<UapmdProjectPluginGraphData> createSerializedProjectGraph(
    const AudioGraphProvider& provider,
    const std::vector<int32_t>& orderedInstanceIds,
    AudioPluginGraph& runtimeGraph,
    const std::function<AudioPluginInstanceAPI*(int32_t instanceId)>& resolveInstance,
    const AudioGraphProvider::PluginStateFileCallback& pluginStateFileCallback = {});
std::unique_ptr<UapmdProjectPluginGraphData> loadSerializedProjectGraph(
    const AudioGraphProvider& provider,
    UapmdProjectPluginGraphData& metadata,
    const std::vector<uint8_t>& bytes);

class AudioGraphProviderRegistry {
public:
    static AudioGraphProviderRegistry create();

    AudioGraphProvider* add(std::unique_ptr<AudioGraphProvider> provider);
    bool remove(AudioGraphProvider* provider);
    void clear();
    const AudioGraphProvider* get(const std::string& graphTypeId) const;
    const AudioGraphProvider* get(const AudioPluginGraph& graph) const;
    std::unique_ptr<AudioPluginGraph> createGraph(
        const std::string& graphTypeId,
        size_t eventBufferSizeInBytes) const;

private:
    std::vector<std::unique_ptr<AudioGraphProvider>> providers_{};
};

} // namespace uapmd
