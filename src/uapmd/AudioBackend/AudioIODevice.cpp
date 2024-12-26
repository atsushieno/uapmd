#include "uapmd/uapmd.hpp"
#include "impl/MiniAudioIODevice.hpp"


uapmd::AudioIODeviceManager* uapmd::AudioIODeviceManager::instance(const std::string &driverName) {
    // We do not support anything but miniaudio, so driverName is not respected here...
    static uapmd::MiniAudioIODeviceManager impl{};
    return &impl;
}
