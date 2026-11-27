#if __APPLE__

#include "PluginFormatAU.hpp"
#include <umppi/umppi.hpp>

remidy::PluginInstanceAUv2::AUUmpInputDispatcher::AUUmpInputDispatcher(remidy::PluginInstanceAUv2 *owner) :
        owner(owner)
{
#define AU_UMP_INPUT_DISPATCHER_UMP_EVENT_LIST_SIZE 65536
    // FIXME: assign MIDI buffer size somewhere
    ump_event_list = (MIDIEventList*) calloc(AU_UMP_INPUT_DISPATCHER_UMP_EVENT_LIST_SIZE, 1);
}
remidy::PluginInstanceAUv2::AUUmpInputDispatcher::~AUUmpInputDispatcher() {
    if (ump_event_list)
        free(ump_event_list);
}

void
remidy::PluginInstanceAUv2::AUUmpInputDispatcher::process(remidy::AudioProcessContext &src) {
    auto& eventIn = src.eventIn();
    auto cur = MIDIEventListInit(ump_event_list, kMIDIProtocol_2_0);
    const auto* bytes = static_cast<const uint8_t*>(eventIn.getMessages());
    const size_t numBytes = eventIn.position();
    for (size_t offset = 0; offset + sizeof(uint32_t) <= numBytes;) {
        auto* words = reinterpret_cast<const UInt32*>(bytes + offset);
        const auto messageType = static_cast<uint8_t>(words[0] >> 28);
        const auto wordCount = umppi::umpSizeInInts(messageType);
        const auto messageSize = static_cast<size_t>(wordCount) * sizeof(uint32_t);
        if (offset + messageSize > numBytes)
            break;
        offset += messageSize;
        // SysEx goes through MusicDeviceSysEx(), ahead of the event list that carries the notes it may retune.
        if (messageType == static_cast<uint8_t>(umppi::MessageType::SYSEX7)) {
            const uint8_t* sysex = nullptr;
            size_t sysexSize = 0;
            if (sysex7_assembler.feed(words[0], words[1], sysex, sysexSize))
                MusicDeviceSysEx((MusicDeviceComponent) owner->instance, sysex, static_cast<UInt32>(sysexSize));
            continue;
        }
        cur = MIDIEventListAdd(ump_event_list, AU_UMP_INPUT_DISPATCHER_UMP_EVENT_LIST_SIZE,
                               cur, 0, static_cast<ByteCount>(wordCount), words);
        if (!cur) {
            owner->logger()->logError("Failed to add UMP events to AudioUnit: event list is full");
            break;
        }
    }
    auto result = MusicDeviceMIDIEventList((MusicDeviceComponent) owner->instance, 0, ump_event_list);
    if (result != noErr) {
        owner->logger()->logError("Failed to add UMP events to AudioUnit: %d", result);
    }
}

#endif
