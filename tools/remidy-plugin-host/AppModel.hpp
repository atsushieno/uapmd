#pragma once

#include <remidy-tooling/PluginScanning.hpp>
#include "remidy-tooling/PluginInstancing.hpp"

namespace uapmd {
    class AppModel {
    public:
        static AppModel& instance();
        remidy_tooling::PluginScanning* pluginScanning;
        std::unique_ptr<remidy_tooling::PluginInstancing> pluginInstancing{};
    };
}
