#pragma once

#include "../AppModel.hpp"
#include "../WebViewProxy.hpp"

namespace uapmd {
    // FIXME: define status codes.
    bool startAudio();
    bool stopAudio();
    bool isAudioPlaying();

    void registerAudioPlayerManagerFeatures(WebViewProxy& proxy);
}
