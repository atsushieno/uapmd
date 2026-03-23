#pragma once

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#include <memory>
#include <string>
#include <vector>

#include "remidy/remidy.hpp"

namespace remidy_tooling {

class PluginFormatManager {
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

    std::vector<remidy::PluginFormat*> formats_{};

public:
    PluginFormatManager();

    std::vector<remidy::PluginFormat*> formats() const;
    const std::vector<remidy::PluginFormat*>& formatView() const { return formats_; }
    void addFormat(remidy::PluginFormat* format);
};

} // namespace remidy_tooling
