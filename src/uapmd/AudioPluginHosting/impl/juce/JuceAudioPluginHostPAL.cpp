#if UAPMD_USE_JUCE_HOSTING

#include "JuceAudioPluginHostPAL.hpp"

#include <juce_audio_plugin_client/juce_audio_plugin_client.h>


remidy::PluginCatalog &uapmd::JuceAudioPluginHostPAL::catalog() {
    // FIXME: implement

    return catalog_;
}

void uapmd::JuceAudioPluginHostPAL::performPluginScanning(bool rescan) {
    // FIXME: implement
}

void
uapmd::JuceAudioPluginHostPAL::createPluginInstance(uint32_t sampleRate, std::string &format, std::string &pluginId,
                                                    std::function<void(std::unique_ptr<AudioPluginNode>,
                                                                       std::string)> &&callback) {
    // FIXME: implement
}

uapmd_status_t uapmd::JuceAudioPluginHostPAL::processAudio(std::vector<remidy::AudioProcessContext *> contexts) {
    // FIXME: implement
    return 0;
}

#endif
