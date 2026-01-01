#pragma once

#include <memory>
#include <string>

#include "uapmd/priv/CommonTypes.hpp"

namespace uapmd {
    class MidiIODevice {

    protected:
        MidiIODevice() = default;
        virtual ~MidiIODevice() = default;

    public:
        static MidiIODevice* instance(std::string driverName = "");

        virtual void addInputHandler(ump_receiver_t receiver, void* userData) = 0;
        virtual void removeInputHandler(ump_receiver_t receiver) = 0;
        virtual void send(uapmd_ump_t* messages, size_t length, uapmd_timestamp_t timestamp) = 0;
        virtual uapmd_status_t start() = 0;
        virtual uapmd_status_t stop() = 0;
    };

    std::shared_ptr<MidiIODevice> createLibreMidiIODevice(std::string apiName,
                                                          std::string deviceName,
                                                          std::string manufacturer,
                                                          std::string version,
#if defined(__APPLE__)
                                                          uint64_t sysExDelayInMicroseconds = 1000
#else
                                                          uint64_t sysExDelayInMicroseconds = 10000
#endif
                                                          );
}
