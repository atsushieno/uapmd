
#include "uapmd/uapmd.hpp"
#include "LibreMidiIODevice.hpp"

void uapmd::LibreMidiIODevice::addCallback(std::function<uapmd_status_t(remidy::AudioProcessContext& data)>&& callback) {
    callbacks.emplace_back(std::move(callback));
}

uapmd_status_t uapmd::LibreMidiIODevice::start() {
    throw std::runtime_error("Not implemented");
}

uapmd_status_t uapmd::LibreMidiIODevice::stop() {
    throw std::runtime_error("Not implemented");
}
