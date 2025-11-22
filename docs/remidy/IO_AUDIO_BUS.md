DRAFT DRAFT DRAFT

# Plugin I/O Ports and Audio/MIDI Bus Configuration

Adjusting audio I/O ports, or buses, as well as event I/O ports/buses is a significant (if still optional) part of the plugin APIs. Their APIs are so diverged per plugin format, but the purpose is in general common.

## Determine What are Needed and What Can be Provided

- Plugin developer defines input audio buses and output audio buses, as well as an input event bus and an output event bus. Each audio bus has a name, and a list of supported audio layouts.
- There will be optional plugin-class-provided main audio input bus and main audio output bus on each plugin instance. They are filled per the rule of its plugin format (mentioned below).
  - Since it is optional, the default "main" bus would be implicitly assumed to be (1) the only I/O bus, or (2) the first entry. But since we don't control the rules of the plugin formats, we can at best "guess" the main bus.
- There will be no option to indicate whether it is a `main` bus or an `aux` bus in each bus:
  - VST3 API has design flaw that `BusInfo` has a flag that indicates whether it is main or not. What happens if there are more than one bus which are "main" ?
  - CLAP, likewise (`has_main_input` in `clap_audio_ports_config_t`).
  - LV2, likewise. It just has a flipped `isSideChain` port property.
  - AudioUnit does not have the concept of "main bus". What is DAW users experience without default main connection? Do their DAW users have to choose the main bus every time?
- User determines which buses are enabled and which channel layout is used for each.
- User app builds audio node graphs, and connects node `X`'s outlet index `a` to node `Y`'s inlet `b`, based on user's operations that connect graphs.
- An audio channel layout is not simply described numerically other than enumerated known ones or without external predefined namings.

Therefore, there will be those types:

- `AudioPluginInstance` exposes list of `AudioBusConfiguration` as `inputBuses` and `outputBuses`.
- `AudioPluginInstance` confirms audio buses setup at `configure()`.
- `AudioChannelLayout` : a named channel layout definition. It needs to provide the channel count. It needs to be referencible and most likely have to be persistable in a portable way as we will have to save it into a music project file. Therefore, it cannot be a simple integer (including an enum value).
- `AudioBusConfiguration` : an audio bus is user-customizable input-or-output port. An audio plugin can have zero or more audio input ports and zero or more audio output ports. One can be the "main" bus on each direction.
  - Each audio bus is named by the plugin developer and assigned its index for each.
  - User customizes its audio channel layout (or the DAW passes its audio input channel layout settings) and either enable or disable each of them (maybe except for the main bus).

## Separation of Concerns

To make everything working, we will have to prepare both audio I/O ports and their connection models. The former is about how we abstract away audio bus configuration (and "event bus" configuration, which we map to and from MIDI 2.0 UMP so far), and the latter is about how we connect them through audio graph. Both are not easy and should not be mixed and made dependent each other, as there could be multiple ways to achieve that.

 After dealing with audio bus complication, every audio node should only get the number of channels on each bus.
 