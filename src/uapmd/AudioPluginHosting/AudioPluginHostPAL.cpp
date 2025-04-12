#include "uapmd/uapmd.hpp"
#include "impl/remidy/RemidyAudioPluginHostPAL.hpp"
#include "impl/juce/JuceAudioPluginHostPAL.hpp"


uapmd::AudioPluginHostPAL* uapmd::AudioPluginHostPAL::instance() {
#if UAPMD_USE_JUCE_HOSTING
    static JuceAudioPluginHostPAL impl{};
    return &impl;
#else
    static RemidyAudioPluginHostPAL impl{};
    return &impl;
#endif
}
