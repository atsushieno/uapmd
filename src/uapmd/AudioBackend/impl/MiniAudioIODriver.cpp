
#include "MiniAudioIODriver.hpp"

void uapmd::MiniAudioIODriver::addAudioCallback(std::function<uapmd_status_t(AudioProcessContext &)> &&callback) {
    throw std::runtime_error("Not implemented");
}

uapmd_status_t uapmd::MiniAudioIODriver::start() {
    throw std::runtime_error("Not implemented");
}

uapmd_status_t uapmd::MiniAudioIODriver::stop() {
    throw std::runtime_error("Not implemented");
}
