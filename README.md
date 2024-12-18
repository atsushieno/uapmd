
# remidy-scan

WIP.

<!--
# What is this?

UAPMD (Universal Audio Plugin MIDI Device) is an audio plugin host that can instantiate arbitrary set of plugins and acts as a virtual MIDI 2.0 UMP device on various platforms.

## Usage

UAPMD consists of two programs:

- uapmd-service: acts as the actual virtual MIDI devices
- uapmd-setup: acts as a configuration tool

### uapmd-service

> $ uapmd-service -audio [audio-config-file] -midi [midi-device-settings-file]

- `-audio`: optional. Specifies audio configuration file that can be created by `uapmd-setup`.
- `-midi`: required. Specifies MIDI device configuration file that can be created by `uapmd-setup`.

### uapmd-setup

> $ uapmd-setup

Launches the GUI by default.

## Code modules

### remidy

`remidy` offers plugin API abstraction layer at lower level that only involves application agnostic audio and event processing. Not very opinionated.

### remidy-tooling

`remidy-tooling` offers higher level API to build audio plugin hosting tools like plugin scanning and instancing in common manner. It is still not much opinionated.

### uapmd

`uapmd` introduces the UMP adoption layer. It is an opinionated layer towards MIDI 2.0 i.e. events are parsed into timed parameter changes and other events on the plugins, as well as save/load states in MIDI-CI property manner.

### uapmd-service

`uapmd-service` works as a virtual MIDI device service that can receive platform UMP inputs (and most likely MIDI 1.0 inputs, translated) to control plugins.
-->

## License and Dependencies

Sources in this repository are released under the MIT license.

There are third-party (and first party) dependency libraries:

- lv2kit (serd, sord, sratom, lilv, zix) - ISC license
- travesty from DPF - ISC license
- moduleinfo from vst3_public_sdk - VST3 public SDK license (BSD-like)
- choc - ISC license
- libremidi - BSD (2-clause), MIT (RtMidi)
- cmidi2 - MIT
- miniaudio - MIT (or public domain)
- cpplocate - MIT
- cxxopts - MIT
- rtlog-cpp - MIT, BSD (2-clause, ReaderWriterQueue)
- saucer - MIT
- webaudio-controls - Apache V2
