#pragma once

#include <uapmd/uapmd.hpp>
#include "uapmd-data/detail/project/UapmdProjectFile.hpp"

namespace uapmd {

struct UapmdProjectPluginGraphEndpointData {
    AudioPluginGraphEndpointType type{AudioPluginGraphEndpointType::Plugin};
    int32_t plugin_index{-1};
    uint32_t bus_index{0};
};

struct UapmdProjectPluginGraphConnectionData {
    int64_t id{0};
    AudioPluginGraphBusType bus_type{AudioPluginGraphBusType::Audio};
    UapmdProjectPluginGraphEndpointData source{};
    UapmdProjectPluginGraphEndpointData target{};
};

class UapmdAudioPluginFullDAGraphData : public UapmdProjectPluginGraphData {
public:
    virtual std::vector<UapmdProjectPluginNodeData> plugins() = 0;
    virtual void addPlugin(UapmdProjectPluginNodeData node) = 0;
    virtual void setPlugins(std::vector<UapmdProjectPluginNodeData> nodes) = 0;
    virtual void clearPlugins() = 0;

    virtual std::vector<UapmdProjectPluginGraphConnectionData> connections() = 0;
    virtual void addConnection(UapmdProjectPluginGraphConnectionData connection) = 0;
    virtual void setConnections(std::vector<UapmdProjectPluginGraphConnectionData> connections) = 0;
    virtual void clearConnections() = 0;

    static std::unique_ptr<UapmdAudioPluginFullDAGraphData> create();
};

} // namespace uapmd
