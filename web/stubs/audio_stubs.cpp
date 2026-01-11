#include <uapmd/priv/devices/AudioIODevice.hpp>
#include <algorithm>

namespace uapmd {

class WebAudioIODeviceManager : public AudioIODeviceManager {
public:
    WebAudioIODeviceManager() : AudioIODeviceManager("webaudio") {}
    ~WebAudioIODeviceManager() override = default;

    void initialize(Configuration&) override {}
    void shutdown() override {}

    std::vector<AudioIODeviceInfo> onDevices() override {
        // No devices in web stub
        return {};
    }

    AudioIODevice* open(int, int, uint32_t) override {
        // No-op device
        return nullptr;
    }

    std::vector<uint32_t> getDeviceSampleRates(const std::string&, AudioIODirections) override {
        // Common sample rates in web contexts
        return { 44100, 48000, 96000 };
    }
};

static WebAudioIODeviceManager g_webAudioManager;

AudioIODeviceManager* AudioIODeviceManager::instance(const std::string&) {
    return &g_webAudioManager;
}

}

