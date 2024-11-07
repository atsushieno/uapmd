
#include "uapmd/uapmd.hpp"
#include "LibreMidiIODevice.hpp"
#include <iostream>

void uapmd::LibreMidiIODevice::addCallback(std::function<uapmd_status_t(remidy::AudioProcessContext& data)>&& callback) {
    callbacks.emplace_back(std::move(callback));
}

uapmd_status_t uapmd::LibreMidiIODevice::start() {
    std::cerr << "Not implemented" << std::endl;
    return 0;
}

uapmd_status_t uapmd::LibreMidiIODevice::stop() {
    std::cerr << "Not implemented" << std::endl;
    return 0;
}
