# What is this?

Remidy aims to provide audio plugin hosting features in a cross-platform and
multi-format manner in liberal licenses (MIT/BSD).

Remidy aims to cover VST3, AudioUnit (on macOS), LV2, and CLAP formats.

UAPMD (Ubiquitous Audio Plugin MIDI Device) is an audio plugin host that can instantiate <del>arbitrary set of plugins</del> <ins>a synth plugin</ins> and acts as a virtual MIDI 2.0 UMP device on various platforms.

Currently, both Remidy and UAPMD target only desktop platforms so far, but if you use my [AAP project](https://github.com/atsushieno/aap-core) those synth plugins already work as UMP devices (you need Android 15 or later that supports [`MidiUmpDeviceService`](https://developer.android.com/reference/kotlin/android/media/midi/MidiUmpDeviceService)).

UAPMD makes it as a GUI tool `uapmd-service` that instantiates one single audio plugin for each virtual UMP device, and translates UMP inputs into event inputs to those in each plugin API.

## Screenshots

I put them on the [wiki](https://github.com/atsushieno/uapmd/wiki) pages.

## What's the point of this tool?

With UAPMD, You do not have to wait for MIDI 2.0 synthesizers in the market; existing audio plugins should work as a virtual MIDI 2.0 device. We have timidity++ or fluidsynth, Microsoft GS wavetable synth, YAMAHA S-YXG etc. for MIDI 1.0. UAPMD will take a similar place for MIDI 2.0.

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

`remidy` offers plugin API abstraction layer at lower level that primarily involves application agnostic audio and event processing. Apart from parameter API, it adopts UMP for event inputs, including parameter support via NRPN (AC, Assignable Controller) and Per-Note AC. It is an opinionated layer towards MIDI 2.0 i.e. events are parsed into timed parameter changes and other events on the plugins.

### remidy-tooling

`remidy-tooling` offers higher level API to build audio plugin hosting tools like plugin scanning and instancing in common manner.
What this layer introduces in practice is a set of filters e.g. various existing specific plugin products and vendors are filtered at "safe for multithreaded access to the plugin API", "plugin scanning requires UI thread", or "crashes remidy" kind of information.
Regarding event streams, they are still not much opinionated.

### uapmd

`uapmd` provides reusable foundation for constructing virtual MIDI 2.0 devices upon plugin hosting layer (only remidy so far). It serves `AllCtrlList` MIDI-CI standard property for plugin parameters as Assignable Controllers (NRPNs), `ProgramList` MIDI-CI standard property for the indexed presets as Program Change, and saves and loads states in MIDI-CI property manner. It is supposed to manage multiple tracks with multiple plugins [not implemented yet].

### uapmd-service

`uapmd-service` works as a virtual MIDI device service that can receive platform UMP inputs (and most likely MIDI 1.0 inputs, translated, depending on the platform) to control plugins.


## License and Dependencies

Sources in this repository are released under the MIT license.

There are third-party (and first party) dependency libraries (git submodules, CMake FetchContent, or directly included):

- [lv2/lv2kit](https://github.com/lv2/lv2kit) (serd, sord, sratom, lilv, zix): the ISC license.
- [travesty](https://github.com/DISTRHO/DPF/tree/main/distrho/src/travesty), part of DISTRHO/DPF: the ISC license
- [free-audio/clap](https://github.com/free-audio/clap) - MIT
- [free-audio/clap-helpers](https://github.com/free-audio/clap-helpers) - MIT
- [steinbergmedia/vst3sdk](https://github.com/steinbergmedia/vst3sdk) - MIT
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
- [cpm-cmake/CPM.cmake](https://github.com/cpm-cmake/CPM.cmake) - MIT
- [ocornut/imgui](https://github.com/ocornut/imgui) - MIT
