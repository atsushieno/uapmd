#pragma once

#include <functional>
#include "uapmd/uapmd.hpp"
#include "AudioIODevice.hpp"

namespace uapmd {

    class DeviceIODispatcher {
    public:
        virtual ~DeviceIODispatcher() = default;

        virtual uapmd_status_t configure(size_t umpBufferSizeInBytes, AudioIODevice* audio = nullptr, MidiIODevice* midiIn = nullptr, MidiIODevice* midiOut = nullptr) = 0;

        virtual AudioIODevice* audio() = 0;
        virtual MidiIODevice* midiIn() = 0;
        virtual MidiIODevice* midiOut() = 0;

        virtual void addCallback(std::function<uapmd_status_t(AudioProcessContext& data)>&& callback) = 0;
        virtual uapmd_status_t start() = 0;
        virtual uapmd_status_t stop() = 0;

        virtual bool isPlaying() = 0;
    };

    DeviceIODispatcher* defaultDeviceIODispatcher();
}
