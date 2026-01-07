#include "uapmd/uapmd.hpp"
#include "impl/RemidyAudioPluginHostPAL.hpp"


uapmd::AudioPluginHostingAPI* uapmd::AudioPluginHostingAPI::instance() {
    static RemidyAudioPluginHostPAL impl{};
    return &impl;
}
