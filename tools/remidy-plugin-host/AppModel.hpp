#pragma once

#include <uapmd/uapmd.hpp>
#include <remidy-tooling/PluginScanning.hpp>
#include "remidy-tooling/PluginInstancing.hpp"

namespace uapmd {
    class AppModel {
        const int32_t ump_buffer_size;
        int32_t sample_rate;
        DeviceIODispatcher dispatcher;
        AudioPluginHostPAL* plugin_host_pal;
        AudioPluginSequencer sequencer;

    public:
        static AppModel& instance();

        AppModel(size_t umpBufferSize, int32_t sampleRate) :
            ump_buffer_size(umpBufferSize), sample_rate(sampleRate),
            plugin_host_pal(AudioPluginHostPAL::instance()),
            sequencer(this->plugin_host_pal),
            dispatcher(umpBufferSize) {
        }

        remidy::PluginCatalog& catalog() { return plugin_host_pal->catalog(); }

        void performPluginScanning(bool rescan) {
            plugin_host_pal->performPluginScanning(rescan);
        }

        std::vector<std::function<void(int32_t instancingId, std::string)>> instancingCompleted{};

        void instantiatePlugin(int32_t instancingId, const std::string_view& format, const std::string_view& pluginId) {
            std::string formatString{format};
            std::string pluginidString{pluginId};
            sequencer.addSimpleTrack(sample_rate, formatString, pluginidString, [&](std::string error) {
                // FIXME: error reporting instead of dumping out here
                if (!error.empty()) {
                    std::string msg = std::format("Instancing ID {}: {}", instancingId, error);
                    remidy::Logger::global()->logError(msg.c_str());
                }
                for (auto& f : instancingCompleted)
                    f(instancingId, error);
            });
        }
    };
}
