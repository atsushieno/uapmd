
#include "choc/text/choc_JSON.h"
#include "AudioPlayerController.hpp"

bool uapmd::startAudio() {
    return AppModel::instance().startAudio() == 0;
}
bool uapmd::stopAudio() {
    return AppModel::instance().stopAudio() == 0;
}
bool uapmd::isAudioPlaying() {
    return AppModel::instance().isAudioPlaying();
}

void uapmd::registerAudioPlayerManagerFeatures(remidy::webui::WebViewProxy& proxy) {
    proxy.registerFunction("remidy_startAudio", [](const std::string_view& args) -> std::string {
        return AppModel::instance().startAudio() ? "true" : "false";
    });
    proxy.registerFunction("remidy_stopAudio", [](const std::string_view& args) -> std::string {
        return AppModel::instance().stopAudio() ? "true" : "false";
    });
    proxy.registerFunction("remidy_isAudioPlaying", [](const std::string_view& args) -> std::string {
        return AppModel::instance().isAudioPlaying() ? "true" : "false";
    });
    proxy.registerFunction("remidy_getSampleRate", [](const std::string_view& args) -> std::string {
        return std::to_string(AppModel::instance().sampleRate());
    });
    proxy.registerFunction("remidy_setSampleRate", [](const std::string_view& args) -> std::string {
        auto newSampleRate = choc::json::parseValue(args).getInt64();
        return AppModel::instance().sampleRate(static_cast<int32_t>(newSampleRate)) ? "true" : "false";
    });
}