#if __APPLE__

#include "PluginFormatAU.hpp"

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
    MIDIEventListAdd(ump_event_list, AU_UMP_INPUT_DISPATCHER_UMP_EVENT_LIST_SIZE,
                     cur, 0, eventIn.position() / sizeof(UInt32), (const UInt32*) eventIn.getMessages());
    auto result = MusicDeviceMIDIEventList((MusicDeviceComponent) owner->instance, 0, ump_event_list);
    if (result != noErr) {
        owner->logger()->logError("Failed to add UMP events to AudioUnit: %d", result);
    }
}

#endif
