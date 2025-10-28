#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "uapmd/uapmd.hpp"
#include "../VirtualMidiDevices/UapmdMidiDevice.hpp"

namespace uapmd {

    class VirtualMidiDeviceController {
        std::unique_ptr<AudioPluginSequencer> sequencer_;
        std::vector<std::shared_ptr<UapmdMidiDevice>> devices_;
        bool audio_running_{false};

        int32_t instantiatePluginOnTrack(int32_t trackIndex,
                                         std::string format,
                                         std::string pluginId,
                                         std::string& error);
        void syncDeviceAssignments();

    public:
        VirtualMidiDeviceController();

        AudioPluginSequencer* sequencer() { return sequencer_.get(); }

        std::shared_ptr<UapmdMidiDevice> createDevice(const std::string& apiName,
                                                      const std::string& deviceName,
                                                      const std::string& manufacturer,
                                                      const std::string& version,
                                                      int32_t trackIndex,
                                                      const std::string& formatName,
                                                      const std::string& pluginId,
                                                      std::string& errorMessage);

        bool removeDevice(int32_t instanceId);

        const std::vector<std::shared_ptr<UapmdMidiDevice>>& devices() const { return devices_; }
    };

}
