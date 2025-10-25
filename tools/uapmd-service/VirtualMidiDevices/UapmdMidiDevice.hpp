#pragma once
#include <functional>
#include <string>

#include "uapmd/uapmd.hpp"
#include "PlatformVirtualMidiDevice.hpp"
#include "midicci/midicci.hpp"

namespace uapmd {

    class UapmdMidiDevice {
        std::string api_name{}, device_name{}, manufacturer{}, version{};

        std::unique_ptr<PlatformVirtualMidiDevice> platformDevice{};
        std::unique_ptr<AudioPluginSequencer> seq{};
        std::map<uint32_t, std::unique_ptr<midicci::musicdevice::MidiCISession>> ci_sessions{};
        std::vector<midicci::musicdevice::MidiInputCallback> ci_input_forwarders{};

        uapmd_status_t maybeLoadConfiguration(std::string& portId);
        uapmd_status_t preparePlugins();

        static void umpReceived(void* context, uapmd_ump_t* ump, size_t sizeInBytes, uapmd_timestamp_t timestamp);
        void umpReceived(uapmd_ump_t* ump, size_t sizeInBytes, uapmd_timestamp_t timestamp);
        int32_t channelToTrack(int32_t group, int32_t channel);
        void setupMidiCISession(int32_t instanceId);

    public:
        UapmdMidiDevice(std::string& apiName, std::string& deviceName, std::string& manufacturer, std::string& version);
        AudioPluginSequencer* sequencer() { return seq.get(); }
        void addPluginTrack(std::string& pluginName, std::string& formatName);
        void addPluginTrackById(const std::string& formatName, const std::string& pluginId,
                                std::function<void(int32_t instanceId, std::string error)> callback);
        void addPluginToTrackById(int32_t trackIndex, const std::string& formatName, const std::string& pluginId,
                                  std::function<void(int32_t instanceId, std::string error)> callback);
        bool removePluginInstance(int32_t instanceId);

        // registers itself as a platform virtual MIDI device service
        uapmd_status_t start();
        // unregisters it from the platform
        uapmd_status_t stop();
    };

}
