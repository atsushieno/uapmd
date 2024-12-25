#pragma once

#include "../AppModel.hpp"
#include "remidy-webui/WebViewProxy.hpp"

namespace uapmd {
    // FIXME: define status codes.
    bool startAudio();
    bool stopAudio();
    bool isAudioPlaying();

    void registerAudioPlayerManagerFeatures(remidy::webui::WebViewProxy& proxy);
}
