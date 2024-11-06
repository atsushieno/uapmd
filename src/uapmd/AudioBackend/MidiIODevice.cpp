
#include "uapmd/uapmd.hpp"
#include "uapmd/priv/MidiIODevice.hpp"
#include "impl/LibreMidiIODevice.hpp"

uapmd::MidiIODevice *uapmd::MidiIODevice::instance(std::string driverName) {
    static LibreMidiIODevice impl{};
    return &impl;
}
