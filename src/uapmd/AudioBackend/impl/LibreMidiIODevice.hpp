#pragma once

#include <uapmd/uapmd.hpp>

namespace uapmd {
    class LibreMidiIODevice : public MidiIODevice {
        std::vector<std::function<uapmd_status_t (AudioProcessContext& data)>> callbacks{};
    public:
        LibreMidiIODevice() = default;
        ~LibreMidiIODevice() override = default;

        void addCallback(std::function<uapmd_status_t (AudioProcessContext& data)>&& callback) override;
        uapmd_status_t start() override;
        uapmd_status_t stop() override;
    };
}