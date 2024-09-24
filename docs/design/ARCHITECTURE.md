# The App Architecture

UniversalAudioPluginMidiDevice consists of two parts:

- `uapmd-setup`: GUI controller that manages audio plugin graph setup. It may support more features. It could be launched in tray at startup, instantiating the "active" (or "resident") setup.
- `uapmd-service`: instantiates the actual virtual MIDI devices.

One UniversalAudioPlugin MIDI device instantiates a set of audio plugin graphs that can be mapped to one or more channels in a group.

[Enhancement] A mapping extension is considered for MPE-enabled synths, where we could map MIDI 2.0 inputs to MPE-based MIDI 1.0 messages and send them to the plugin, therefore no need for multi-channel spans.

The channel details could be provided through MIDI-CI Property Exchange ProgramList Resource (M2-108-UM) etc. We can expose which audio plugins are used through those properties.

Saving and loading virtual device settings is done at client side.

## MIDI API

UAPMD works with the following API with the target platform so far:

- Linux (kernel 6.5 or later): ALSA 1.2.10 or later
- MacOS (14.0 or later): CoreMIDI
- Windows (probably 11): Windows MIDI Service

Other platform APIs do not support MIDI 2.0 UMP ports.

The actual platform MIDI interaction is done through libremidi.

## Audio Plugin Graph Configuration

Every channel can be mapped to a plugin graph. A plugin graph consists of one or more audio plugins, chained.

### Audio Plugin Formats

We plan to work with these audio plugin formats:

- LV2
- VST3 (Travesty)
- AudioUnit V2
- AudioUnit V3
- CLAP

Our hosting support is limited to the following features:

- plugin listing
- instantiation
- configuring audio ports to some extent
- mapping MIDI inputs to plugin parameters
- process MIDI inputs and get audio outputs
- get and set state as per request via MIDI-CI Property Exchange

Namely, we do not particularly support plugin UI.

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

We need to support up to 256 audio plugin "tracks" where each of them maps to an audio plugin graph.

For this purpose we need a realtime sequencer-like audio engine.

an audio plugin track contains an audio plugin graph, and an audio plugin graph (in practice) contains an instrument, optionally followed by effect plugins. In the simplest version, the audio plugin graph is just a sequential list of audio plugins. MIDI input port to the instrument, and stereo connections on primary audio bus between the rest of the plugins are assumed.

[Enhancement] we can have a fully-featured audio plugin graph.

## Implementation

For virtual MIDI 2.0 device part it makes use of libremidi. It should work on Linux 6.5 or later, MacOS 14.0 or later, and Windows probably 11 or later (`src/service/VirtualMidiDevices`).

For audio plugin hosting, we're currently evaluating libossia. It can be Tracktion Engine, but we're not sure if it is good enough to process MIDI events appropriately dispatched to each track. Also it is concerning that the Tracktion developers are quite negative on MIDI 2.0.

For `uapmd-setup` GUI we plan to use Tauri 2.0 Web UI, and connect to the service (`src/service/Controller` and `client`).
