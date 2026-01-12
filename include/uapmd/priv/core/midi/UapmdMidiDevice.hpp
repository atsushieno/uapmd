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
#include "../uapmd-core.hpp"

namespace uapmd {
    class UapmdMidiDevice {
        std::string api_name{};

        SequencerFeature* sequencer{};
        int32_t instance_id{-1};
        int32_t track_index{-1};
        uint8_t ump_group{0xFF};

        std::shared_ptr<MidiIOFeature> midi_device{};

        static void umpReceived(void* context, uapmd_ump_t* ump, size_t sizeInBytes, uapmd_timestamp_t timestamp);
        void umpReceived(uapmd_ump_t* ump, size_t sizeInBytes, uapmd_timestamp_t timestamp);
        void setupMidiCISession();
        void teardownOutputHandler();

        std::unique_ptr<UapmdMidiCISessions> uapmd_sessions{};

    public:
        UapmdMidiDevice(std::shared_ptr<MidiIOFeature> midiDevice,
                        SequencerFeature* sequencer,
                        int32_t instanceId,
                        int32_t trackIndex,
                        std::string apiName,
                        std::string deviceName,
                        std::string manufacturer,
                        std::string version);

        ~UapmdMidiDevice();

        MidiIOFeature* midiIO() { return midi_device.get(); }

        int32_t instanceId() const { return instance_id; }
        int32_t trackIndex() const { return track_index; }
        void trackIndex(int32_t newIndex) { track_index = newIndex; }
        uint8_t group() const { return ump_group; }
        void group(uint8_t groupId) { ump_group = groupId; }

        void initialize();
    };

}
