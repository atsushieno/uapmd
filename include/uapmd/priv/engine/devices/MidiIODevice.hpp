#pragma once

#include <memory>
#include <string>

#include "../../core/CommonTypes.hpp"

namespace uapmd {
    class MidiIODevice : public MidiIOFeature {

    protected:
        MidiIODevice() = default;
        virtual ~MidiIODevice() = default;

    public:
        static MidiIODevice* instance(std::string driverName = "");
    };

    std::shared_ptr<MidiIODevice> createLibreMidiIODevice(std::string apiName,
                                                          std::string deviceName,
                                                          std::string manufacturer,
                                                          std::string version,
#if defined(__APPLE__)
                                                          uint64_t sysExDelayInMicroseconds = 20
#else
                                                          uint64_t sysExDelayInMicroseconds = 10000
#endif
                                                          );

    bool midiApiSupportsUmp(const std::string& apiName);
}
