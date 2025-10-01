#pragma once

#include <uapmd/uapmd.hpp>
#include <remidy-tooling/PluginScanTool.hpp>
#include "../../include/uapmd/priv/sequencer/AudioPluginSequencer.hpp"
#include <format>
#include <thread>

namespace uapmd {
    class AppModel {
        AudioPluginSequencer sequencer_;
        remidy_tooling::PluginScanTool pluginScanTool_;
        std::atomic<bool> isScanning_{false};

    public:
        static void instantiate();
        static AppModel& instance();
        static void cleanupInstance();
        AppModel(size_t audioBufferSizeInFrames, size_t umpBufferSizeInBytes, int32_t sampleRate);

        AudioPluginSequencer& sequencer() { return sequencer_; }
        remidy_tooling::PluginScanTool& pluginScanTool() { return pluginScanTool_; }
        bool isScanning() const { return isScanning_; }

        std::vector<std::function<void(int32_t instancingId, int32_t instanceId, std::string)>> instancingCompleted{};
        std::vector<std::function<void(bool success, std::string error)>> scanningCompleted{};

        void instantiatePlugin(int32_t instancingId, const std::string_view& format, const std::string_view& pluginId) {
            std::string formatString{format};
            std::string pluginIdString{pluginId};
            sequencer_.instantiatePlugin(formatString, pluginIdString, [this, instancingId](int32_t instanceId, std::string error) {
                // FIXME: error reporting instead of dumping out here
                if (!error.empty()) {
                    std::string msg = std::format("Instancing ID {}: {}", instancingId, error);
                    remidy::Logger::global()->logError(msg.c_str());
                }
                for (auto& f : instancingCompleted)
                    f(instancingId, instancingId, error);
            });
        }

        void performPluginScanning(bool forceRescan = false);
    };
}
