#include "uapmd/uapmd.hpp"

#if defined(__ANDROID__)
#  include "OboeAudioIODevice.hpp"
#elif defined(__EMSCRIPTEN__)
#  include "WebAudioWorkletIODevice.hpp"
#else
#  include "MiniAudioIODevice.hpp"
#endif

uapmd::AudioIODeviceManager* uapmd::AudioIODeviceManager::instance(const std::string &driverName) {
    (void) driverName;
#if defined(__ANDROID__)
    static uapmd::OboeAudioIODeviceManager impl{};
#elif defined(__EMSCRIPTEN__)
    static uapmd::WebAudioWorkletIODeviceManager impl{};
#else
    static uapmd::MiniAudioIODeviceManager impl{};
#endif
    return &impl;
}
