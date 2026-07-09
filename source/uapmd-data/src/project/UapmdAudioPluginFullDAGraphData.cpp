#include "UapmdAudioPluginFullDAGraphData.hpp"

namespace uapmd {

class UapmdAudioPluginFullDAGraphDataImpl : public UapmdAudioPluginFullDAGraphData {
    std::string graph_type_{};
    std::filesystem::path external_file_{};
    std::vector<UapmdProjectPluginNodeData> plugins_{};
    std::vector<AudioGraphNodeDescriptor> generic_nodes_{};
    std::vector<UapmdProjectPluginGraphConnectionData> connections_{};
    std::map<std::string, std::string> properties_{};

public:
    std::string graphType() override { return graph_type_; }
    std::filesystem::path externalFile() override { return external_file_; }
    std::vector<UapmdProjectPluginNodeData> plugins() override { return plugins_; }
    std::vector<AudioGraphNodeDescriptor> genericNodes() override { return generic_nodes_; }
    std::vector<UapmdProjectPluginGraphConnectionData> connections() override { return connections_; }
    std::map<std::string, std::string> properties() override { return properties_; }

    void graphType(const std::string& type) override { graph_type_ = type; }
    void externalFile(const std::filesystem::path& f) override { external_file_ = f; }
    void addPlugin(UapmdProjectPluginNodeData node) override { plugins_.push_back(std::move(node)); }
    void setPlugins(std::vector<UapmdProjectPluginNodeData> nodes) override { plugins_ = std::move(nodes); }
    void clearPlugins() override { plugins_.clear(); }
    void addGenericNode(AudioGraphNodeDescriptor node) override { generic_nodes_.push_back(std::move(node)); }
    void setGenericNodes(std::vector<AudioGraphNodeDescriptor> nodes) override { generic_nodes_ = std::move(nodes); }
    void clearGenericNodes() override { generic_nodes_.clear(); }
    void addConnection(UapmdProjectPluginGraphConnectionData connection) override { connections_.push_back(std::move(connection)); }
    void setConnections(std::vector<UapmdProjectPluginGraphConnectionData> connections) override { connections_ = std::move(connections); }
    void clearConnections() override { connections_.clear(); }
    void properties(std::map<std::string, std::string> values) override { properties_ = std::move(values); }
};

std::unique_ptr<UapmdAudioPluginFullDAGraphData> UapmdAudioPluginFullDAGraphData::create() {
    return std::make_unique<UapmdAudioPluginFullDAGraphDataImpl>();
}

} // namespace uapmd
