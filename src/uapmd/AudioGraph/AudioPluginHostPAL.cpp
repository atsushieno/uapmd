#include "uapmd/uapmd.hpp"
#include "../AudioPluginAPI/RemidyAudioPluginHost.hpp"


uapmd::AudioPluginHostingAPI* uapmd::AudioPluginHostingAPI::instance() {
    static RemidyAudioPluginHost impl{};
    return &impl;
}
