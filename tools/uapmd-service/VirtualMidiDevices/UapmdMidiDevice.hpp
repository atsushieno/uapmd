#pragma once
#include <string>

#include "uapmd/uapmd.hpp"
#include "PlatformVirtualMidiDevice.hpp"

namespace uapmd {

    class UapmdMidiDevice {
        std::string deviceName{}, manufacturer{}, version{};

        std::unique_ptr<PlatformVirtualMidiDevice> platformDevice{};
        std::unique_ptr<SequenceProcessor> audioPluginHost;
        uapmd_status_t maybeLoadConfiguration(std::string& portId);
        uapmd_status_t preparePlugins();

        static void umpReceived(void* context, uapmd_ump_t* ump, size_t sizeInBytes, uapmd_timestamp_t timestamp);
        void umpReceived(uapmd_ump_t* ump, size_t sizeInBytes, uapmd_timestamp_t timestamp);
        int32_t channelToTrack(int32_t group, int32_t channel);

    public:
        UapmdMidiDevice(std::string& deviceName, std::string& manufacturer, std::string& version);

        // registers itself as a virtual MIDI device service
        uapmd_status_t start();
        // unregisters it from the platform
        uapmd_status_t stop();
    };

}
