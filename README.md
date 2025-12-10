# What is this?

There are two primary components on this repository:

- `remidy` aims to provide audio plugin hosting features in a cross-platform and multi-format manner in liberal licenses (MIT/BSD). It supports VST3, AudioUnit (on macOS), LV2, and CLAP formats.
- `uapmd` (Ubiquitous Audio Plugin MIDI Device) is an audio plugin host that can instantiate an arbitrary set of plugins and acts as a virtual MIDI 2.0 UMP device on various platforms (multiple tracks do not mean they work in parallel yet).

At user developers perspective, there are two primary GUI tools:

- `remidy-plugin-host` is a simple plugin host that you can list the installed plugins, instantiate, and process audio with simple MIDI 2.0 keyboard.
- `uapmd-service` instantiates one single audio plugin for each virtual MIDI 2.0 device, and translates UMP inputs into event inputs to those in each plugin API, as well as exposing some plugin features using MIDI-CI property exchange.

## Screenshots

I put them on the [wiki](https://github.com/atsushieno/uapmd/wiki) pages.

## What's the point of these tools?

With UAPMD, You do not have to wait for MIDI 2.0 synthesizers in the market; existing audio plugins should work as a virtual MIDI 2.0 device. We have timidity++ or fluidsynth, Microsoft GS wavetable synth, YAMAHA S-YXG etc. for MIDI 1.0. UAPMD will take a similar place for MIDI 2.0.

Currently, both Remidy and UAPMD target only desktop platforms so far, but if you use my [AAP project](https://github.com/atsushieno/aap-core) those synth plugins already work as UMP devices (you need Android 15 or later that supports [`MidiUmpDeviceService`](https://developer.android.com/reference/kotlin/android/media/midi/MidiUmpDeviceService)).

## Usage

This repository consists of two primary executables:

- `remidy-plugin-host` is an audio plugin host
- `uapmd-service` is a plugin host that maps plugin instances to platform virtual MIDI 2.0 devices

There are supplemental tools for diagnosing problems we encounter.

### uapmd-service

The virtual MIDI 2.0 device service controller. Currently the command line options are hacky:

> $ uapmd-service (plugin-name) (format-name) (api-name)

`plugin-name` is match by `std::string::contains()` within display name, case-sensitive.

`format-name` is one of `VST3` `AU`, `LV2`, or `CLAP`.

`api-name` so far accepts only `PIPEWIRE` (on Linux) to use PipeWire, and uses default available API otherwise.

### remidy-plugin-host

The plugin host. No particular command line options exist.

### remidy-scan

`remidy-scan` is a tool to query and enumerate locally installed plugins, and stores the results to `(local app data)/remidy-tooling/plugin-list-cache.json` (`local app data` depends on the platform).

## Documentation

ALL docs under [`docs`](docs) are supposed to describe design investigation and thoughts.

We are moving quick and may not reflect current state of union, or describe our plans correctly.

There are some notable docs:

- [Plugin catalog (listing) and instantiation](docs/remidy/PLUGIN_ID_AND_CATALOG.md)
- [State](docs/remidy/STATE.md)
- [GUI support and main thread constraints](docs/remidy/GUI_SUPPORT.md)
- [Parameters](docs/remidy/PARAMETERS.md)
- [Presets](docs/remidy/PRESETS.md)

## Code modules

### remidy

`remidy` offers plugin API abstraction layer at lower level that primarily involves application agnostic audio and event processing. Apart from parameter API, it adopts UMP for event inputs, including parameter support via NRPN (AC, Assignable Controller) and Per-Note AC. It is an opinionated layer towards MIDI 2.0 i.e. events are parsed into timed parameter changes and other events on the plugins.

### remidy-tooling

`remidy-tooling` offers higher level API to build audio plugin hosting tools like plugin scanning and instancing in common manner.
What this layer introduces in practice is a set of filters e.g. various existing specific plugin products and vendors are filtered at "safe for multithreaded access to the plugin API", "plugin scanning requires UI thread", or "crashes remidy" kind of information.

### uapmd

`uapmd` provides reusable foundation for constructing virtual MIDI 2.0 devices upon plugin hosting layer (only remidy so far). It serves `AllCtrlList` MIDI-CI standard property for plugin parameters as Assignable Controllers (NRPNs), `ProgramList` MIDI-CI standard property for the indexed presets as Program Change, and saves and loads states in MIDI-CI property manner. It is supposed to manage multiple tracks with multiple plugins [not implemented yet].

### uapmd-service

`uapmd-service` works as a virtual MIDI device service that can receive platform UMP inputs (and most likely MIDI 1.0 inputs, translated, depending on the platform) to control plugins.


## License and Dependencies

Sources in this repository are released under the MIT license.

There are third-party (and first party) dependency libraries (git submodules, CMake FetchContent, or directly included):

- [lv2/lv2kit](https://github.com/lv2/lv2kit) (serd, sord, sratom, lilv, zix): the ISC license.
- [free-audio/clap](https://github.com/free-audio/clap) - MIT
- [free-audio/clap-helpers](https://github.com/free-audio/clap-helpers) - MIT
- [steinbergmedia/vst3sdk](https://github.com/steinbergmedia/vst3sdk) - MIT
- [Tracktion/choc](https://github.com/Tracktion/choc/): the ISC license.
- [celtera/libremidi](https://github.com/celtera/libremidi) - BSD (2-clause), MIT (RtMidi)
- [atsushieno/cmidi2](atsushieno/cmidi2) - MIT
- [atsushieno/midicci](atsushieno/midicci) - MIT
  - [zlib-ng/zlib-ng] - Zlib license.
- [mackron/miniaudio](https://github.com/mackron/miniaudio) - MIT (or public domain)
- [cginternals/cpplocate](https://github.com/cginternals/cpplocate): the MIT license.
- [jarro2783/cxxopts](https://github.com/jarro2783/cxxopts): the MIT license.
- [cameron314/concurrentqueue](https://github.com/cameron314/concurrentqueue) - BSD (2-clause)
- [cjappl/rtlog-cpp](https://github.com/cjappl/rtlog-cpp): the MIT license.
    - for submodules see their [LICENSE.md](https://github.com/cjappl/rtlog-cpp/blob/main/LICENSE.md) (modified BSD, MIT)
- [cpm-cmake/CPM.cmake](https://github.com/cpm-cmake/CPM.cmake) - MIT
- [ocornut/imgui](https://github.com/ocornut/imgui) - MIT
