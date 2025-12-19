#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>

// FIXME: remove these undefs once we sort out name conflicts
#undef JR_TIMESTAMP_TICKS_PER_SECOND
#undef MIDI_2_0_RESERVED
#include "midicci/midicci.hpp"
#include "uapmd/uapmd.hpp"

namespace uapmd {
    class AudioPluginSequencer;
    class UapmdMidiCISessions;

    class UapmdMidiDevice {
        // FIXME: remove this hack
        friend class UapmdMidiCISessions;

        std::string api_name{};

        AudioPluginSequencer* sequencer{};
        int32_t instance_id{-1};
        int32_t track_index{-1};
        uint8_t ump_group{0xFF};

        std::unique_ptr<PlatformVirtualMidiDevice> platform_device{};
        bool output_handler_registered{false};

        static void umpReceivedTrampoline(void* context, uapmd_ump_t* ump, size_t sizeInBytes, uapmd_timestamp_t timestamp);
        void umpReceived(uapmd_ump_t* ump, size_t sizeInBytes, uapmd_timestamp_t timestamp);
        void setupMidiCISession();
        void teardownOutputHandler();

        std::unique_ptr<UapmdMidiCISessions> uapmd_sessions{};

    public:
        UapmdMidiDevice(std::unique_ptr<PlatformVirtualMidiDevice> platformMidiDevice,
                        AudioPluginSequencer* sequencer,
                        int32_t instanceId,
                        int32_t trackIndex,
                        std::string apiName,
                        std::string deviceName,
                        std::string manufacturer,
                        std::string version);

        ~UapmdMidiDevice();

        AudioPluginSequencer* sequencerPtr() { return sequencer; }
        int32_t instanceId() const { return instance_id; }
        int32_t trackIndex() const { return track_index; }
        void trackIndex(int32_t newIndex) { track_index = newIndex; }
        uint8_t group() const { return ump_group; }
        void group(uint8_t groupId) { ump_group = groupId; }

        void initialize();

        uapmd_status_t start();
        uapmd_status_t stop();
    };

}
