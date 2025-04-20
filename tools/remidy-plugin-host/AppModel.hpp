#pragma once

#include <uapmd/uapmd.hpp>
#include <remidy-tooling/PluginScanTool.hpp>
#include "../../include/uapmd/priv/sequencer/AudioPluginSequencer.hpp"

namespace uapmd {
    class AppModel {
        AudioPluginSequencer sequencer_;

    public:
        static AppModel& instance();
        AppModel(size_t audioBufferSizeInFrames, size_t umpBufferSizeInBytes, int32_t sampleRate);

        AudioPluginSequencer& sequencer() { return sequencer_; }

        void instantiatePlugin(int32_t instancingId, const std::string_view& format, const std::string_view& pluginId) {
            std::string formatString{format};
            std::string pluginIdString{pluginId};
            sequencer_.instantiatePlugin(instancingId, formatString, pluginIdString);
        }
    };
}
