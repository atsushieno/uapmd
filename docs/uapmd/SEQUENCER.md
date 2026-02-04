
# Sequencer Engine

UAPMD is essentially an audio engine just like the ones in a DAW. It handles multiple audio plugin tracks, where a track essentially contains an audio graph (either linear or complicated ones), and it is supposed to merge them once all the tracks are processed and apply master effects, which is also an audio graph.

The audio engine has to work in two ways:

- live rendering: the audio engine processes audio in realtime manner. They have to be strictly on time and cannot either go ahead of time or go behind the deadline.
- static (offline) rendering: the audio engine processes audio in static manner. They can go ahead than realtime processing, and there is no deadline. They still have to process accordingly. In the ideal world, they render identical outputs for identical inputs (but they might not).

## Current API Design Before Refactoring

### `RealtimeSequencer`

It was supposed to be a live-rendering version of the audio engine. 

It works like the only facade for the entire audio engine.

### `SequencerEngine`

It was supposed to be a static audio engine that can process audio without time constraints.

It is still used by remidy-apply, but the console tool is not really functional for now.

### `DeviceIODispatcher`, `AudioIODevice`, and `MidiIODevice`

`DeviceIODispatcher` collects device I/O functionality (i.e. `AudioIODevice` and `MidiIODevice`), which is essential to the live rendering engine.

Conceptually they can also exist as virtual static entities, but they are so far specific to `RealtimeSequencer`.

### `AudioPluginTrack`

A `SequencerEngine` holds a list of `AudioPluginTrack` instances. It currently exists for:

- a control point for bypassing (mute)
- a control point for freezing (not implemented)
- resolved to a UMP group (Function Block)
- (UMP) event I/O (enqueue / register output callback)

### `AudioPluginGraph`

An `AudioPluginTrack` contains an `AudioPluginGraph`.

It is designed to be pure virtual and can be replaced by any audio graph implementation. Currently only a simple linear graph exists, but something like Tracktion Graph could be applied (IF that JUCE module is applicable within our normal library paradigm).

There could be a master track which should also contain a plugin graph, but currently it does not exist.

### `AudioPluginNode`

An `AudioPluginGraph` holds a list of `AudioPluginNode`s.

There is no abstraction on audio graph nodes. The inputs and outputs are represented as `DeviceIODispatcher` and `RealtimeSequencer` handles it. There is no other nodes such as mixer nodes or volume controller nodes. They could be part of audio graph implementation details. Our simple audio graph does not support it.

This node also holds an audio plugin instance and UAPMD UMP I/O mappers.

### `AudioPluginHostingAPI` and `AudioPluginInstanceAPI`

These classes abstacts away audio plugin hosting API. While UAPMD is primarily targeting Remidy API, we try to make rooms for other audio plugin hosting libraries.

Note that We still have to resort to some types in remidy regardless of the plugin hosting API (such as `AudioProcessContext` for audio processing inputs).

An `AudioPluginNode` contains one `AudioPluginInstanceAPI`, which is supposed to be instantiated by `AudioPluginHostingAPI::createPluginInstance()`. But currently it returns `AudioPluginNode`.
