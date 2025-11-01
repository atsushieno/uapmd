
## midicci

For MIDI-CI messaging we use midicci, which is mere port of ktmidi-ci and ktmidi-ci-tool from Kotlin to C++ by some agentic AI coders. Whenever we need more features, we will bring them in [ktmidi](https://github.com/atsushieno/ktmidi/) first and then let those AI coders to port.

## Audio engine, plugin tracks, and plugin instances

There is one single `AudioPluginSequencer`, which manages multiple `AudioPluginTrack`s. One `AudioPluginTrack` so far holds a simple `AudioPluginGraph` which also includes I/O nodes.

UAPMD so far creates one virtual MIDI 2.0 (UMP) device for each plugin instance, whereas the actual audio I/O happens ultimately on the single `AudioPluginSequencer`. The UMP devices act only as the controllers to the plugins.

This avoids need for MIDI mappings unlike typical DAWs.

## Function Blocks

Although separate UMP device for each plugin instance perfectly works for MIDI-CI property features, we should not necesarily need separate event stream for each plugin. Instead, we could use Function Blocks and use "group" field to designate particular plugin instance to control.

However current midicci API is not designed to make it doable. We need appropriate Function Block mappings.
