#include "uapmd/uapmd.hpp"
#include "impl/MiniAudioIODevice.hpp"

uapmd::AudioIODevice* uapmd::AudioIODevice::instance(const std::string& deviceName, const std::string& driverName) {
    static MiniAudioIODevice impl{deviceName};
    return &impl;
}
