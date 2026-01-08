
DRAFT DRAFT DRAFT

# Audio Processing in Remidy

## From SequencerEngine to AudioPluginNode

- `SequencerEngine` contains a set of `AudioPluginTrack`s and takes `SequenceProcessContext` when playing a song, in each audio processing cycle.
- `AudioPluginTrack` contains an `AudioPluginGraph` and takes `AudioProcessContext` in each audio processing cycle.
- `AudioPluginGraph` connects one or more `AudioPluginNode`s. It is supposed to offer implementation independent `process()`.
  - So far it supports only linear audio plugin node chain (`appendNodeSimple()`).
- `AudioPluginNode` is a plugin node. It is host-implementation-agnostic.
  - The actual hosting API implements whatever the overall plugin abstraction API requires through `AudioPluginHostPAL`.

## Plugins' Audio Processing Requirements

- Usually a `DeviceIODispatcher` runs its audio I/O cycle and through its callbacks it kicks `SequencerEngine::process(SequenceProcessContext&)` to get outputs to the audio and MIDI devices.
  - There can also be offline rendering runner that calls `SequencerEngine::process(SequenceProcessContext&)` in non-realtime manner.
- `SequencerEngine` collects generated audio and MIDI2 outputs that `AudioPluginTrack`s generate, and mixes them into its final outputs.
- In some plugin formats such as VST3, "process" function might alter the pointers to audio output buffers. We should let audio processors in the `AudioPluginGraph` adjust the buffers and pass them to the next plugin in the chain.
