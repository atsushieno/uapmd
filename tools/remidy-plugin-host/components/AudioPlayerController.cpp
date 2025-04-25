
#include "choc/text/choc_JSON.h"
#include "AudioPlayerController.hpp"
#include "../AppModel.hpp"

void uapmd::registerAudioPlayerManagerFeatures(remidy::webui::WebViewProxy& proxy) {
    proxy.registerFunction("remidy_startAudio", [](const std::string_view& args) -> std::string {
        return AppModel::instance().sequencer().startAudio() ? "true" : "false";
    });
    proxy.registerFunction("remidy_stopAudio", [](const std::string_view& args) -> std::string {
        return AppModel::instance().sequencer().stopAudio() ? "true" : "false";
    });
    proxy.registerFunction("remidy_isAudioPlaying", [](const std::string_view& args) -> std::string {
        return AppModel::instance().sequencer().isAudioPlaying() ? "true" : "false";
    });
    proxy.registerFunction("remidy_getSampleRate", [](const std::string_view& args) -> std::string {
        return std::to_string(AppModel::instance().sequencer().sampleRate());
    });
    proxy.registerFunction("remidy_setSampleRate", [](const std::string_view& args) -> std::string {
        auto newSampleRate = choc::json::parseValue(args).getInt64();
        return AppModel::instance().sequencer().sampleRate(static_cast<int32_t>(newSampleRate)) ? "true" : "false";
    });
}