#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "midicci/midicci.hpp" // include before anything that indirectly includes X.h
#include "uapmd/uapmd.hpp"
#include "../../../include/uapmd/priv/midi/UapmdMidiDevice.hpp"

namespace uapmd {

    class VirtualMidiDeviceController {
        AudioPluginSequencer* sequencer_;  // Non-owning pointer to shared sequencer
        std::vector<std::shared_ptr<UapmdMidiDevice>> devices_;
        bool audio_running_{false};

        int32_t instantiatePluginOnTrack(int32_t trackIndex,
                                         std::string format,
                                         std::string pluginId,
                                         std::string& error);
        void syncDeviceAssignments();

    public:
        VirtualMidiDeviceController(AudioPluginSequencer* sharedSequencer);

        AudioPluginSequencer* sequencer() { return sequencer_; }

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
