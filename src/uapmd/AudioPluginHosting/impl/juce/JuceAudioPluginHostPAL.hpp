#pragma once

#if UAPMD_USE_JUCE_HOSTING

#include "uapmd/uapmd.hpp"

namespace uapmd {

    class JuceAudioPluginHostPAL : public AudioPluginHostPAL {
        remidy::PluginCatalog catalog_{};

    public:
        remidy::PluginCatalog & catalog() override;
        void performPluginScanning(bool rescan) override;
        void createPluginInstance(uint32_t sampleRate,
                                  std::string &format,
                                  std::string &pluginId,
                                  std::function<void(std::unique_ptr<AudioPluginNode>, std::string)>&& callback
                                  ) override;
        uapmd_status_t processAudio(std::vector<remidy::AudioProcessContext *> contexts) override;
    };

}

#endif

