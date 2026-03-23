#include "remidy-tooling/priv/PluginFormatManager.hpp"

namespace remidy_tooling {

PluginFormatManager::PluginFormatManager() {
#if ANDROID
    aap_ = remidy::PluginFormatAAP::create();
    if (aap_)
        formats_.push_back(aap_.get());
#elif defined(__EMSCRIPTEN__)
    webclap_ = remidy::PluginFormatWebCLAP::create();
    if (webclap_)
        formats_.push_back(webclap_.get());
#elif defined(__APPLE__) && TARGET_OS_IPHONE
    au_ = remidy::PluginFormatAU::create();
    if (au_)
        formats_.push_back(au_.get());
#else
    vst3_ = remidy::PluginFormatVST3::create(vst3SearchPaths_);
    lv2_ = remidy::PluginFormatLV2::create(lv2SearchPaths_);
    clap_ = remidy::PluginFormatCLAP::create(clapSearchPaths_);
#if __APPLE__
    au_ = remidy::PluginFormatAU::create();
#endif

    if (clap_)
        formats_.push_back(clap_.get());
    if (lv2_)
        formats_.push_back(lv2_.get());
#if __APPLE__
    if (au_)
        formats_.push_back(au_.get());
#endif
    if (vst3_)
        formats_.push_back(vst3_.get());
#endif
}

std::vector<remidy::PluginFormat*> PluginFormatManager::formats() const {
    return formats_;
}

void PluginFormatManager::addFormat(remidy::PluginFormat* format) {
    if (format)
        formats_.push_back(format);
}

} // namespace remidy_tooling
