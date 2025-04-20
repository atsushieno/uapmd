#pragma once

#include <functional>

namespace uapmd {
    class MidiIODevice {
        class Impl;
        Impl* impl;

    protected:
        MidiIODevice() = default;
        virtual ~MidiIODevice() = default;

    public:
        static MidiIODevice* instance(std::string driverName = "");

        virtual void addCallback(std::function<uapmd_status_t(AudioProcessContext&)>&& data) = 0;
        virtual uapmd_status_t start() = 0;
        virtual uapmd_status_t stop() = 0;
    };
}
