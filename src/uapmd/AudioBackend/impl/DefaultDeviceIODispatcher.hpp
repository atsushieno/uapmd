#pragma once

#include <atomic>
#include <optional>
#include <thread>
#include <vector>
#include "uapmd/uapmd.hpp"

namespace uapmd {
    class DefaultDeviceIODispatcher : public DeviceIODispatcher {
        std::vector<std::function<uapmd_status_t(AudioProcessContext& data)>> callbacks{};
        AudioIODevice* audio_{};
        MidiIODevice* midi_in{};
        MidiIODevice* midi_out{};
        uapmd_ump_t* queued_inputs{};
        std::atomic<size_t> next_ump_position{0};
        size_t ump_buffer_size_in_bytes{0};
        std::optional<std::thread::id> audio_thread_id{};

        uapmd_status_t runCallbacks(AudioProcessContext& data);

    public:
        DefaultDeviceIODispatcher();
        ~DefaultDeviceIODispatcher() override;

        uapmd_status_t configure(size_t umpBufferSizeInBytes, AudioIODevice* audio = nullptr, MidiIODevice* midiIn = nullptr, MidiIODevice* midiOut = nullptr) override;

        AudioIODevice* audio() override;
        MidiIODevice* midiIn() override;
        MidiIODevice* midiOut() override;

        void addCallback(std::function<uapmd_status_t(AudioProcessContext& data)>&& callback) override;
        uapmd_status_t start() override;
        uapmd_status_t stop() override;

        bool isPlaying() override;
    };
}

