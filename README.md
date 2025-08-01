# What is this?

Remidy aims to provide audio plugin hosting features in cross-platform and
multi-format manner in liberal licenses (MIT/BSD).

Remidy aims to cover VST3, AudioUnit (on macOS), LV2, and CLAP formats.

UAPMD (Ubiquitous Audio Plugin MIDI Device) <del>is</del> <ins>aims to become</ins> an audio plugin host that can instantiate <del>arbitrary set of plugins</del> <ins>a synth plugin</ins> and acts as a virtual MIDI 2.0 UMP device on various platforms.

UAPMD so far makes it as a console tool `uapmd-service` that instantiates one single audio plugin and translates UMP inputs into event inputs for each plugin API.

## What's the point of this tool?

You will not have to wait for MIDI 2.0 synthesizers in the market. Existing audio plugins work as a virtual MIDI 2.0 device. We have timidity++ or fluidsynth, Microsoft GS wavetable synth, YAMAHA S-YXG etc. for MIDI 1.0. UAPMD will take the similar place for MIDI 2.0.

## Usage

UAPMD consists of two programs:

- uapmd-service: acts as the actual virtual MIDI devices
- (not available yet) uapmd-setup: acts as a configuration tool

There are supplemental tools for diagnosing problems we encounter.

### uapmd-service

Currently the command line options are hacky:

> $ uapmd-service plugin-name (format-name) (api-name)

`plugin-name` is match by `std::string::contains()` within display name, case-sensitive.

`format-name` is one of `VST3` `AU`, `LV2`, or `CLAP`.

`api-name` so far accepts only `PIPEWIRE` (on Linux) to use PipeWire, and uses default available API otherwise.

<!--

> $ uapmd-service -audio [audio-config-file] -midi [midi-device-settings-file]

- `-audio`: optional. Specifies audio configuration file that can be created by `uapmd-setup`.
- `-midi`: required. Specifies MIDI device configuration file that can be created by `uapmd-setup`.

### uapmd-setup

> $ uapmd-setup

Launches the GUI by default.
-->

### remidy-plugin-host

A hacky WIP dogfooding plugin host.

### remidy-scan

remidy-scan is a tool to query and enumerate locally installed plugins.


## Code modules

### remidy

`remidy` offers plugin API abstraction layer at lower level that only involves application agnostic audio and event processing. Apart from parameter API, it adopts UMP for event inputs, including parameter support via NRPN (AC, Assignable Controller) and Per-Note AC. It is an opinionated layer towards MIDI 2.0 i.e. events are parsed into timed parameter changes and other events on the plugins.

### remidy-tooling

`remidy-tooling` offers higher level API to build audio plugin hosting tools like plugin scanning and instancing in common manner.
What this layer introduces in practice is a set of highly product dependent filterings e.g. various existing specific plugin products and vendors are filtered at "safe for multi-threaded plugin API access", "plugin scanning requires UI thread", or "crashes remidy" kind of information.
Regarding event stream it is still not much opinionated.

### uapmd

`uapmd` provides reusable foundation for constructing virtual MIDI 2.0 devices upon plugin hosting layer (only remidy so far). It saves and loads states in MIDI-CI property manner [not implemented yet]. It is supposed to manage multiple tracks with multiple plugins [not implemented yet].

### uapmd-service

`uapmd-service` works as a virtual MIDI device service that can receive platform UMP inputs (and most likely MIDI 1.0 inputs, translated, depending on the platform) to control plugins.


## License and Dependencies

Sources in this repository are released under the MIT license.

There are third-party (and first party) dependency libraries (git submodules, CMake FetchContent, or directly included):

- [lv2/lv2kit](https://github.com/lv2/lv2kit) (serd, sord, sratom, lilv, zix): the ISC license.
- [travesty](https://github.com/DISTRHO/DPF/tree/main/distrho/src/travesty), part of DISTRHO/DPF: the ISC license
- [moduleinfo](https://github.com/steinbergmedia/vst3_public_sdk/tree/master/source/vst/moduleinfo), part of steinbergmedia/vst3_public_sdk: VST 3 SDK public_sdk License (BSD like)
  (modified to remove dependencies on `vst3_pluginterfaces` which is GPLed)
- [free-audio/clap](https://github.com/free-audio/clap): MIT
- [free-audio/clap-helpers](https://github.com/free-audio/clap-helpers): MIT
- [Tracktion/choc](https://github.com/Tracktion/choc/): the ISC license.
- [celtera/libremidi](https://github.com/celtera/libremidi) - BSD (2-clause), MIT (RtMidi)
- [atsushieno/cmidi2](atsushieno/cmidi2) - MIT
- [atsushieno/midicci](atsushieno/midicci) - MIT
- [mackron/miniaudio](https://github.com/mackron/miniaudio) - MIT (or public domain)
- [cginternals/cpplocate](https://github.com/cginternals/cpplocate): the MIT license.
- [jarro2783/cxxopts](https://github.com/jarro2783/cxxopts): the MIT license.
- [cameron314/concurrentqueue](https://github.com/cameron314/concurrentqueue) - BSD (2-clause)
- [cjappl/rtlog-cpp](https://github.com/cjappl/rtlog-cpp): the MIT license.
    - for submodules see their [LICENSE.md](https://github.com/cjappl/rtlog-cpp/blob/main/LICENSE.md) (modified BSD, MIT)
- [saucer/saucer](https://github.com/saucer/saucer) - MIT
- [g200kg/webaudio-controls](https://github.com/g200kg/webaudio-controls) - Apache V2
- [cpm-cmake/CPM.cmake](https://github.com/cpm-cmake/CPM.cmake) - MIT
