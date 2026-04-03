#pragma once

#include "uapmd-data/detail/project/UapmdProjectFile.hpp"

namespace uapmd {

class UapmdProjectPluginListGraphData : public UapmdProjectPluginGraphData {
public:
    virtual std::vector<UapmdProjectPluginNodeData> plugins() = 0;
    virtual void addPlugin(UapmdProjectPluginNodeData node) = 0;
    virtual void setPlugins(std::vector<UapmdProjectPluginNodeData> nodes) = 0;
    virtual void clearPlugins() = 0;

    static std::unique_ptr<UapmdProjectPluginListGraphData> create();
};

} // namespace uapmd
