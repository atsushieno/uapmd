#include "UapmdAudioPluginFullDAGraphData.hpp"

namespace uapmd {

class UapmdAudioPluginFullDAGraphDataImpl : public UapmdAudioPluginFullDAGraphData {
    std::string graph_type_{};
    std::filesystem::path external_file_{};
    std::vector<UapmdProjectPluginNodeData> plugins_{};
    std::vector<UapmdProjectPluginGraphConnectionData> connections_{};

public:
    std::string graphType() override { return graph_type_; }
    std::filesystem::path externalFile() override { return external_file_; }
    std::vector<UapmdProjectPluginNodeData> plugins() override { return plugins_; }
    std::vector<UapmdProjectPluginGraphConnectionData> connections() override { return connections_; }

    void graphType(const std::string& type) override { graph_type_ = type; }
    void externalFile(const std::filesystem::path& f) override { external_file_ = f; }
    void addPlugin(UapmdProjectPluginNodeData node) override { plugins_.push_back(std::move(node)); }
    void setPlugins(std::vector<UapmdProjectPluginNodeData> nodes) override { plugins_ = std::move(nodes); }
    void clearPlugins() override { plugins_.clear(); }
    void addConnection(UapmdProjectPluginGraphConnectionData connection) override { connections_.push_back(std::move(connection)); }
    void setConnections(std::vector<UapmdProjectPluginGraphConnectionData> connections) override { connections_ = std::move(connections); }
    void clearConnections() override { connections_.clear(); }
};

std::unique_ptr<UapmdAudioPluginFullDAGraphData> UapmdAudioPluginFullDAGraphData::create() {
    return std::make_unique<UapmdAudioPluginFullDAGraphDataImpl>();
}

} // namespace uapmd
