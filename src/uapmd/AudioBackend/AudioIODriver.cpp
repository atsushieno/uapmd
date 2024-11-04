#include "uapmd/uapmd.hpp"
#include "impl/MiniAudioIODriver.hpp"

uapmd::AudioIODriver* uapmd::AudioIODriver::instance(std::string driverName) {
    static MiniAudioIODriver impl{};
    return &impl;
}
