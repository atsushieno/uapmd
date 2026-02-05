#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>

#include "midicci/midicci.hpp"
#include "uapmd/uapmd.hpp"

namespace uapmd {
    class UapmdFunctionBlock {
        AudioPluginNode* plugin_node;
        uint8_t ump_group{0xFF}; // invalid

        std::shared_ptr<MidiIOFeature> midi_device{};

        static void umpReceived(void* context, uapmd_ump_t* ump, size_t sizeInBytes, uapmd_timestamp_t timestamp);
        void umpReceived(uapmd_ump_t* ump, size_t sizeInBytes, uapmd_timestamp_t timestamp);

        std::unique_ptr<UapmdMidiCISession> uapmd_sessions{};

    public:
        UapmdFunctionBlock(std::shared_ptr<MidiIOFeature> midiDevice,
                        AudioPluginNode* pluginNode,
                        std::string deviceName,
                        std::string manufacturer,
                        std::string version);

        ~UapmdFunctionBlock();

        MidiIOFeature* midiIO() { return midi_device.get(); }

        int32_t instanceId() const { return plugin_node->instanceId(); }
        uint8_t group() const { return ump_group; }
        void group(uint8_t groupId) { ump_group = groupId; }

        void initialize();
    };

}
