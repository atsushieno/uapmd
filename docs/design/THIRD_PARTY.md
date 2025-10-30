# Choices of third party libraries

## MIDI 2.0 I/O

We use libremidi because none of others offers the same feature set.

- It eases development in decent cross platform manner.
- It supports MIDI 2.0 consistently.

Rust: There are too many delusive MIDI libraries that only takes name and implements nothing.

## MIDI 2.0 UMP / MIDI-CI message processing

We ended up porting ktmidi to C++. ktmidi supports all of Profile Configuration, Property Exchange, and Process Inquiry messages, as well as session communication.

[midi2-dev/ni-midi2](https://github.com/midi2-dev/ni-midi2) is left quite incomplete state and they don't work on it anymore.

juce_midi_ci does good job too, but feature wise it's still incomplete compared to ktmidi. Also licensing matters.

When we need complicated UMP processing maybe we use cmidi2 (ni-midi2 does not even process SysEx8 messages).

## Audio I/O

- We use miniaudio. It is cross-platform ready, has its own audio graph if we want to use (we don't for now though).
- choc might be an option too, but I'm unsure if its complicated buffer converters achieve good performance.
  - Their audio block dispatcher would have been usable if they were not against MIDI 2.0.
    There is no way to dispatch UMPs in their API.

## Audio plugin hosting

- We have our own multi-format hosting library `remidy`, as no one built such one in a liberal license.
  - We use vst3sdk for VST3.
    - We used to use Travesty from DPF, but we have no reason to avoid vst3sdk anymore since it is relicensed to MIT at v3.8.0.
  - We use lv2kit (lilv, serd, sord, sratom, zix) for LV2.
    - LVTK3 might bring in benefits, I just did not make decision to depend on "under heavily development" status of the library (also no desire to migrate from its meson to our CMake, I'm tired of doing that at lv2kit).
  - We use the API headers and clap-helpers for CLAP.
- Other options (and reason to not choose):
  - juce_audio_plugin_client
    - the simplest solution for single track
    - the problem is that it is basically a commercial software (AGPL for library).
    - we will need multi-track hosting and it needs extra work.
    - attempt to use it as a library without success because of unusual library structure (with JUCE we cannot build uapmd as a normal library, can be only JUCE module)
  - tracktion_engine
    - multi-track ready
    - the licensing situation is not any better than JUCE (we need multiple layers of GPL/AGPL)
    - not a safe option as the developers and the community are quite negative against MIDI 2.0.
  - Carla
    - simple solution for single track
    - When I was investigating it contained JUCE and it is still unclear if we can use it in liberal licenses.

## GUI

- We use ImGui for now
