#include <uapmd/priv/devices/MidiIODevice.hpp>

namespace uapmd {
bool midiApiSupportsUmp(const std::string&) {
    // Web MIDI 2.0 not wired yet; report unsupported
    return false;
}
}

