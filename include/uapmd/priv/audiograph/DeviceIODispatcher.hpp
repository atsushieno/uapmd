#pragma once

#include "AudioIODevice.hpp"
#include "MidiIODevice.hpp"

namespace uapmd {
    // Integrates MIDI I/O into audio I/O callbacks and dispatches the received data to user-provided callbacks,
    // then send resulting MIDI outputs to the designated MIDI output when those callbacks are done.
    class DeviceIODispatcher {
        class Impl;
        Impl* impl;

    public:
        explicit DeviceIODispatcher(size_t umpBufferSizeInBytes, AudioIODevice* audioDriver = AudioIODeviceManager::instance()->activeDefaultDevice(), MidiIODevice* midiDriver = nullptr);
        ~DeviceIODispatcher();

        AudioIODevice* audioDevice();
        MidiIODevice* midiDevice();

        // add a user callback that is invoked whenever audio device callback is invoked, or
        // `runCallback()` is manually invoked.
        // It is in general not good idea to expect `data` to stay consistent across multiple callbacks;
        // they are processed in order and previous callbacks might replace data content.
        void addCallback(std::function<uapmd_status_t(AudioProcessContext& data)>&& callback);
        // starts the I/O device callbacks.
        uapmd_status_t start();
        // stops the I/O device callbacks.
        uapmd_status_t stop();

        bool isPlaying();

        // Manually runs the callbacks that were registered by `addCallback()`.
        // IF you do not start any audio IO callbacks, you can use this to manually invoke the callbacks
        // to perform what could happen if the audio I/O callback happened e.g. for testing purpose.
        // In other words, whoever invokes this function needs to process the resulting data by themselves.
        uapmd_status_t manuallyRunCallbacks(AudioProcessContext& data);
    };
}

