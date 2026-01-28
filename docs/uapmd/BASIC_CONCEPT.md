
# Basic Concept

UAPMD is based on a hypothesis that a set of MIDI 2.0 devices and sequence data can be used to achieve what modern DAWs (namely their audio engine and song data) achieve today. Ideally, MIDI 2.0 Container Format (which does not exist yet) should be usable to represent any sequence data.

Most DAWs support multiple audio plugin formats. And to commonize their diverse features, they have some audio playback engine whose features resemble what MIDI 2.0 features.

They are mostly based off of MIDI 1.0, with higher data resolution, more instructions such as per-note controllers, then some features like states, factory/user presets, parameter metadata retrieval, etc.

MIDI 2.0 is not for audio samples. It is only an Instrument Digital Interface. Though typical audio tracks on a DAW are set of positioned audio clips, where each clip consists of audio sample file with some adjustment (time stretch etc.). They can be regarded as "instruments" and thus still subject to instructions by MIDI controllers. It is actually how audio effect plugins (directly) control audio tracks.

## What MIDI 2.0 cannot replace

MIDI 2.0 is still an interface that interacts with instruments and it is NOT for expressing the instruments themselves (unless they are fully described within the MIDI protocol).

When we edit audio plugin presets (or more precisely, states), we mostly resort to their GUI. MIDI 2.0 is not there to alter this workflow. Yet MIDI 2.0 lets you save and load state of an instrument, just like an audio plugin does in its context track.

Other individual aspects of plugin API features that cannot be achived in MIDI 2.0:

- Dynamic parameter list changes: VST3 has `IComponentHandler::restartComponent()`, AUv3 has update notifications on `AUParameterTree`, AUv2 can notify value changes in AudioUnit properties, CLAP has `clap_host.request_restart()`, but there is nothing comparable to it in MIDI 2.0.
- Range of Program, Bank MSB, and Bank LSB: Preset index can often span beyond 128, so simply assigning the index to program number will limit the range of supported presets. On the other hand, VST3 preset banks can also go beyond 128, and it's up to each plugin product whether they prefer larger bank count over preset index, or larger preset index over the bank count.
  - UAPMD resolves this by using the 7th. bit of Bank MSB to indicate whether the field is used for index (the flag is 1) or for the bank MSB (the flag is 0).
