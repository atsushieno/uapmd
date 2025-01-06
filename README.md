
# remidy-scan

Remidy aims to provide audio plugin hosting features in cross-platform and
multi-format manner in liberal licenses (MIT/BSD).

Remidy aims to cover VST3, AudioUnit (on macOS) and LV2 formats.

remidy-scan is a tool to query and enumerate locally installed plugins.

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

`remidy-tooling` offers higher level API to build audio plugin hosting tools like plugin scanning and instancing in common manner.
What this layer introduces in practice is a set of highly product dependent filterings e.g. various existing specific plugin products and vendors are filtered at "safe for multi-threaded plugin API access", "plugin scanning requires UI thread", or "crashes remidy" kind of information.
Regarding event stream it is still not much opinionated.

### uapmd

`uapmd` introduces the UMP adoption layer. It is an opinionated layer towards MIDI 2.0 i.e. events are parsed into timed parameter changes and other events on the plugins, as well as save/load states in MIDI-CI property manner.

### uapmd-service

`uapmd-service` works as a virtual MIDI device service that can receive platform UMP inputs (and most likely MIDI 1.0 inputs, translated) to control plugins.
-->

## License and Dependencies

Sources in this repository are released under the MIT license.

There are third-party (and first party) dependency libraries:

- [lv2/lv2kit](https://github.com/lv2/lv2kit) (serd, sord, sratom, lilv, zix): the ISC license.
- [travesty](https://github.com/DISTRHO/DPF/tree/main/distrho/src/travesty), part of DISTRHO/DPF: the ISC license
- [moduleinfo](https://github.com/steinbergmedia/vst3_public_sdk/tree/master/source/vst/moduleinfo), part of steinbergmedia/vst3_public_sdk: VST 3 SDK public_sdk License (BSD like)
  (modified to remove dependencies on `vst3_pluginterfaces` which is GPLed)
- [Tracktion/choc](https://github.com/Tracktion/choc/): the ISC license.
- [celtera/libremidi](https://github.com/celtera/libremidi) - BSD (2-clause), MIT (RtMidi)
- [atsushieno/cmidi2](atsushieno/cmidi2) - MIT
- [mackron/miniaudio](https://github.com/mackron/miniaudio) - MIT (or public domain)
- [cginternals/cpplocate](https://github.com/cginternals/cpplocate): the MIT license.
- [jarro2783/cxxopts](https://github.com/jarro2783/cxxopts): the MIT license.
- [jpcima/ring-buffer](https://github.com/jpcima/ring-buffer): BSL-1.0 license
- [cjappl/rtlog-cpp](https://github.com/cjappl/rtlog-cpp): the MIT license.
    - for submodules see their [LICENSE.md](https://github.com/cjappl/rtlog-cpp/blob/main/LICENSE.md) (modified BSD, MIT)
- [saucer/saucer](https://github.com/saucer/saucer) - MIT
- [g200kg/webaudio-controls](https://github.com/g200kg/webaudio-controls) - Apache V2
