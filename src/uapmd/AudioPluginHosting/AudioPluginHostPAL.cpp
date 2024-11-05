#include "uapmd/priv/AudioPluginHostPAL.hpp"
#include "impl/RemidyAudioPluginHostPAL.hpp"


uapmd::AudioPluginHostPAL* uapmd::AudioPluginHostPAL::instance() {
    static RemidyAudioPluginHostPAL impl{};
    return &impl;
}
