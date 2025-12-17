# The App Architecture

There is an audio plugin "sequencer" which is the audio engine in uapmd-app.

A sequencer consists of sequences. One audio plugin track instantiates an audio plugin graph that can be mapped to one or more channels in a group.

[Enhancement] A mapping extension is considered for MPE-enabled synths, where we could map MIDI 2.0 inputs to MPE-based MIDI 1.0 messages and send them to the plugin, therefore no need for multi-channel spans.

The channel details could be provided through MIDI-CI Property Exchange ProgramList Resource (M2-108-UM) etc. We can expose which audio plugins are used through those properties.

Saving and loading virtual device settings is done at the client side.

## MIDI API

UAPMD works with the following API with the target platform so far:

- Linux (kernel 6.5 or later): PipeWire 1.4 or later if available, or ALSA 1.2.10 or later.
- MacOS (14.0 or later): CoreMIDI
- Windows (probably 11): Windows MIDI Services

Other platform APIs do not support MIDI 2.0 UMP ports (on desktop).

Regarding all those platform APIs above, the actual platform MIDI interaction is done through `libremidi`.

## Audio Plugin Graph Configuration

Every channel can be mapped to a plugin graph. A plugin graph consists of one or more audio plugins, chained.

### Audio Plugin Formats

We plan to work with these audio plugin formats:

- LV2
- VST3
- AudioUnit (V2 and V3)
- CLAP

Our hosting support is limited to the following features:

- plugin listing
- instantiation
- configuring audio ports to some extent
- mapping MIDI inputs to plugin parameters
- process MIDI inputs and get audio outputs
- get and set state as per request via MIDI-CI Property Exchange

Namely, we do not particularly support plugin UI.

### Multi-format ready plugin state

There are handful of concerns around saving and restoring audio plugins:

- States should not be bound to local PC
  e.g. my plugin state saved on my work PC should be recoverable on my home PC.
- Saved states should be loadable on different OSes
  e.g. my plugin state saved on Linux should be loadable on Windows.
- Saved states should be loadable *across* plugin formats
  e.g. my plugin state saved in VST3 should be loadable in AU, LV2, or CLAP.

## Audio Configuration

By default, we use platform's default audio I/O device settings.

[Enhancement] It needs audio device configuration setup. The client has configuration UI. The configuration is passed to service in a configuration setup string and is saved as a file.

[Enhancement] A virtual MIDI device setup could contain the audio setup.

[Enhancement] Audio configuration can be named and made selectable for each virtual MIDI device.

## Controller features

- `CreateDevice(deviceName, manufacturer, version)` : creates a new virtual MIDI 2.0 device.
- `RegisterPluginChannels(device, pluginID)` : an audio plugin is registered to the device.
- `MapPluginChannel(device, pluginID, group, channel)` : map a channel in a group to a plugin.

## Service workflow

- `OpenPort(portId)` : Open a virtual MIDI device. It does the following work:
  - `CreateVirtualDevice(portId)` create a virtual device
  - `CreatePluginHost`: create a plugin host with the current audio settings
  - `PreparePlugins`: load all the registered plugins and prepare everything to play

[Enhancement] The service can add or remove plugins while they are instantiated.

## Audio Plugin Hosting

The audio plugin sequencer engine needs to support multiple audio plugin "tracks" where each of them maps to an audio plugin graph.

For this purpose we need a realtime sequencer-like audio engine.

An audio plugin track contains an audio plugin graph, and an audio plugin graph (in practice) contains an instrument, optionally followed by effect plugins. In the simplest version, the audio plugin graph is just a sequential list of audio plugins. MIDI input port to the instrument, and stereo connections on primary audio bus between the rest of the plugins are assumed.

We can register as many audio plugins as possible, but one "device" would not be able to support more than 128 plugins because each plugin needs at least one input group and one output group.

[Enhancement] we can have a fully-featured audio plugin graph.

## Implementation

### Choices of Third Party Libraries

See [THIRD_PARTY.md](THIRD_PARTY.md).
