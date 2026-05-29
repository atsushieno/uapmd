#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>

#include "uapmd/uapmd.hpp"

namespace uapmd {
    class UapmdMidiCISession;
    class UapmdNodeUmpOutputMapper;

    class UapmdFunctionBlock {
        AudioPluginNodeFeature* plugin_node_feature;
        uint8_t ump_group{0xFF}; // invalid

        std::shared_ptr<MidiIOFeature> midi_device{};
        std::unique_ptr<UapmdNodeUmpOutputMapper> ump_output_mapper_{};

        static void umpReceived(void* context, uapmd_ump_t* ump, size_t sizeInBytes, uapmd_timestamp_t timestamp);
        void umpReceived(uapmd_ump_t* ump, size_t sizeInBytes, uapmd_timestamp_t timestamp);

        std::unique_ptr<UapmdMidiCISession> uapmd_sessions{};

    public:
        UapmdFunctionBlock(std::shared_ptr<MidiIOFeature> midiDevice,
                        AudioPluginNodeFeature* pluginNodeFeature,
                        std::string deviceName,
                        std::string manufacturer,
                        std::string version);

        ~UapmdFunctionBlock();

        MidiIOFeature* midiIO() { return midi_device.get(); }
        uint8_t group() const { return ump_group; }
        void group(uint8_t groupId) { ump_group = groupId; }

        // Detach the UMP output mapper (unregisters parameter listeners from the plugin)
        // while the plugin instance is still alive. Must be called before the plugin is
        // freed, because DeviceState::device shared_ptrs may outlive the engine tracks.
        void detachOutputMapper();

        void initialize();
    };

}
