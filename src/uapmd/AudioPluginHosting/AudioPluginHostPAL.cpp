#include "uapmd/uapmd.hpp"
#include "impl/RemidyAudioPluginHostPAL.hpp"


uapmd::AudioPluginHostPAL* uapmd::AudioPluginHostPAL::instance() {
    static RemidyAudioPluginHostPAL impl{};
    return &impl;
}
