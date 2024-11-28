#pragma once

#include <remidy-tooling/PluginScanning.hpp>

namespace uapmd {
    class AppModel {
    public:
        static AppModel& instance();
        remidy_tooling::PluginScanning* pluginScanning;
    };
}
