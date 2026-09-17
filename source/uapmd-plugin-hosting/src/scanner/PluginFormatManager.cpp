#include "../../include/uapmd-plugin-hosting/detail/scanner/PluginFormatManager.hpp"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#include "remidy/remidy.hpp"
#include "../plugin-api/RemidyPluginFormat.hpp"

namespace uapmd_plugin_hosting {

class PluginFormatManager::Impl {
    std::vector<std::string> vst3SearchPaths_{};
    std::vector<std::string> lv2SearchPaths_{};
    std::vector<std::string> clapSearchPaths_{};

#if ANDROID
    std::unique_ptr<remidy::PluginFormatAAP> aap_;
#elif defined(__EMSCRIPTEN__)
    std::unique_ptr<remidy::PluginFormatWebCLAP> webclap_;
#elif defined(__APPLE__) && TARGET_OS_IPHONE
    std::unique_ptr<remidy::PluginFormatAU> au_;
#else
    std::unique_ptr<remidy::PluginFormatVST3> vst3_;
    std::unique_ptr<remidy::PluginFormatLV2> lv2_;
    std::unique_ptr<remidy::PluginFormatCLAP> clap_;
#if __APPLE__
    std::unique_ptr<remidy::PluginFormatAU> au_;
#endif
#endif

    // The AudioPluginFormat adapters over the remidy formats above.
    std::vector<std::unique_ptr<RemidyPluginFormat>> builtins_{};

    void addBuiltIn(remidy::PluginFormat* format) {
        if (!format)
            return;
        builtins_.emplace_back(std::make_unique<RemidyPluginFormat>(format));
        formats_.push_back(builtins_.back().get());
    }

public:
    std::vector<AudioPluginFormat*> formats_{};

    Impl() {
#if ANDROID
        aap_ = remidy::PluginFormatAAP::create();
        addBuiltIn(aap_.get());
#elif defined(__EMSCRIPTEN__)
        webclap_ = remidy::PluginFormatWebCLAP::create();
        addBuiltIn(webclap_.get());
#elif defined(__APPLE__) && TARGET_OS_IPHONE
        au_ = remidy::PluginFormatAU::create();
        addBuiltIn(au_.get());
#else
        vst3_ = remidy::PluginFormatVST3::create(vst3SearchPaths_);
        lv2_ = remidy::PluginFormatLV2::create(lv2SearchPaths_);
        clap_ = remidy::PluginFormatCLAP::create(clapSearchPaths_);
#if __APPLE__
        au_ = remidy::PluginFormatAU::create();
#endif

        addBuiltIn(clap_.get());
        addBuiltIn(lv2_.get());
#if __APPLE__
        addBuiltIn(au_.get());
#endif
        addBuiltIn(vst3_.get());
#endif
    }
};

PluginFormatManager::PluginFormatManager() : impl_(std::make_unique<Impl>()) {
}

PluginFormatManager::~PluginFormatManager() = default;

std::vector<AudioPluginFormat*> PluginFormatManager::formats() const {
    return impl_->formats_;
}

const std::vector<AudioPluginFormat*>& PluginFormatManager::formatView() const {
    return impl_->formats_;
}

void PluginFormatManager::addFormat(AudioPluginFormat* format) {
    if (format)
        impl_->formats_.push_back(format);
}

} // namespace uapmd_plugin_hosting
