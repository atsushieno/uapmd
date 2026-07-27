#pragma once

#include <memory>
#include <string>
#include <vector>

#include "uapmd-midi-service/uapmd-midi-service.hpp"

namespace uapmd {
    // A platform endpoint identity is intentionally opaque to callers. `id` is
    // stable for the lifetime of the port enumeration and must be used for
    // selection; `displayName` is for UI only and is not necessarily unique.
    struct MidiPortInfo {
        std::string id;
        std::string displayName;
    };

    class MidiIODevice : public MidiIOFeature {

    protected:
        MidiIODevice() = default;
        ~MidiIODevice() override = default;

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

    // Enumerate and open physical/software platform endpoints. These are not
    // UAPMD virtual devices; callers can pass their virtual endpoint IDs to
    // the excludedPortIds argument when building a selector.
    std::vector<MidiPortInfo> getMidiInputPorts(
        const std::vector<std::string>& excludedPortIds = {});
    std::vector<MidiPortInfo> getMidiOutputPorts(
        const std::vector<std::string>& excludedPortIds = {});
    std::shared_ptr<MidiIOFeature> openLibreMidiInputPort(const std::string& portId);
    std::shared_ptr<MidiIOFeature> openLibreMidiOutputPort(const std::string& portId);

    bool midiApiSupportsDynamicUmpEndpoints(const std::string& apiName);
}
