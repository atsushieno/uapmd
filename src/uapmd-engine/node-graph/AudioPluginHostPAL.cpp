#include "uapmd/uapmd.hpp"
#include "../plugin-api/RemidyAudioPluginHost.hpp"


uapmd::AudioPluginHostingAPI* uapmd::AudioPluginHostingAPI::instance() {
    static RemidyAudioPluginHost impl{};
    return &impl;
}
