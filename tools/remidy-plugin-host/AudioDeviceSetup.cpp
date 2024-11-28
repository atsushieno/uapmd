#include "AudioDeviceSetup.hpp"

uapmd::DevicesInterop uapmd::getDevices() {
    DevicesInterop ret {
            .audioIn = { AudioInDeviceInterop{ .id = "0_0", .name = "default audio in from C++" } },
            .audioOut = { AudioOutDeviceInterop{ .id = "0_1", .name = "default audio out from C++" } },
            .midiIn = { MidiInDeviceInterop{ .id = "0_0", .name = "stub midi in from C++" } },
            .midiOut = { MidiOutDeviceInterop{ .id = "0_1", .name = "stub midi out from C++" } }
    };
    return ret;
}
