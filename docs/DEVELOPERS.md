
# Developer’s Guide

## Project Structure & Module Organization

In the latest source tree, the following description applies to the `source` directory.

- `remidy`: plugin API abstraction (VST3/AU/LV2/CLAP backends).
- `remidy-gui`: common GUI helper that is required by plugin formats (such as `ContainerWindow`)
- `uapmd-addin-core` : the UAPMD addin system. Many features are implemented as addins.
- `uapmd-plugin-hosting`: commonized scanning/instancing utilities, and the plugin hosting API (`AudioPluginHostingAPI` / `AudioPluginInstanceAPI`) plus its `remidy`-backed implementation.
- `uapmd-midi-service`: virtual MIDI 2.0 device foundation (function block manager, UMP mapping).
- `uapmd-graph`: audio graph and graph nodes.
- `uapmd-data`: sequencer data structures.
- `uapmd-file`: platform abstraction utility for file dialog and file system.
- `uapmd-engine`: sequencer engine.
- `uapmd-mir`: MIR support module.
- `tools/`: tools
  - `uapmd-app`: an example DAW-like sequencer that also serves virtual UMP devices, for dogfooding
  - `uapmd-app-model`: model API for uapmd-app, to be shared with C API and bindings
  - `uapmd-scan`: standalone plugin scanner (same engine as the app's `--scan-only` mode)
  - `uapmd-apply`: console tool to statically renderer UAPMD project to audio file.
  - `remidy-gui-shared`: some uapmd-specific shared code to implement GUI features (it used to be shared between multiple apps, but not nowaday)
- `external/`: Third‑party dependencies (submodules/FetchContent).
- `cmake/`: CMake build helpers

### remidy

`remidy` offers plugin API abstraction layer at lower level that primarily involves application agnostic audio and event processing. Apart from parameter API, it adopts UMP for event inputs, including parameter support via NRPN (AC, Assignable Controller) and Per-Note AC. It is an opinionated layer towards MIDI 2.0 i.e. events are parsed into timed parameter changes and other events on the plugins.

### uapmd-addin-core

`uapmd-addin-core` implements UAPMD's addin system foundation. We aim to provide UAPMD features as addins, as long as possible, where each feature can be (optionally) loaded dynamically, and enabled/disabled at runtime.

### uapmd-plugin-hosting

`uapmd-plugin-hosting` offers higher level API to build audio plugin hosting tools like plugin scanning and instancing in the common manner.
What this layer introduces in practice is a set of filters; various existing specific plugin products and vendors are filtered by "safe for multithreaded access to the plugin API," "plugin scanning requires the UI thread," or "crashes remidy" kind of information.

### uapmd-graph

`uapmd-graph` provides some audio graph implementations that can be used to build DAW track's realtime audio filter chain. It provides a DAG (directional acyclic graph) implementation and a simple linear graph. Its graph manipulation is performed in a realtime-safe manner.

It also provides some built-in graph nodes such as gain, channel splitter, and analyzer nodes. They are based on Web Audio API to not feel awkward due to the proprietary design.

### uapmd-midi-service

`uapmd-midi-service` provide reusable foundation for constructing virtual MIDI 2.0 devices upon the plugin hosting layer. It serves the following MIDI-CI standard properties:

- `AllCtrlList` for plugin parameters as Assignable Controllers (NRPNs)
- `ProgramList` for the indexed presets as Program Change
- `State` to save and load the plugin states.

### uapmd-data

`uapmd-data` offers the project data model such as tracks, clips, audio clips, MIDI2 clips, plugin states, and audio graphs.
Then it provides saving and loading of the project file specified as the UAPMD project data format, packaged in `.uapmdz` file.
The `uapmdz` format is designed to be close to the (not-yet-published) MIDI 2.0 Container File format (we don't know its details more than what the MIDI Association blog posts describe).

It also provides the document edit model so that DAW edit events can be registered and fired. The edit model comes with undo/redo engine and commands.

### uapmd-file

It is a utility library that provides cross-platform document API as a file system API alternative.

### uapmd-engine

`uapmd-engine` provides UAPMD's sequencer functionality.

goes one step further to establish the premise that there is single audio processing backend, multiple MIDI 2.0 devices, audio graphs, and so on to make everything in usable form.

### uapmd-mir

`uapmd-mir` provides some additional functionality that are based on MIR (music information retrieval) libraries. The features are e.g., source-separated audio track imports, pitch detection from audio tracks, beat and time signature detection from audio tracks. They depend on third-party libraries. Still experimental. It should be also noted that these tasks can also be performed by connected AI agents via JS/MCP.

### uapmd-app

`uapmd-app` is a plugin host that you can list the installed plugins, instantiate plugins, process audio with a UMP keyboard, adjust parameters, select presets, launch the GUI, save, and restore the states. It also exposes those plugins as platform virtual MIDI 2.0 devices, translating UMP inputs into event inputs to those in each plugin API, as well as exposing some plugin features using MIDI-CI property exchange.

It works with MIDI 1.0 inputs (translated, depending on the platform) to control plugins.


## CMake Build options

These CMake options controls enablement of some modules:

- `UAPMD_ENABLE_MIR`: enables MIR module (experimental and not-yet-working module)
  - `UAPMD_ENABLE_BASIC_PITCH` : enables basic-pitch support (opt-in Apache2-licensed module)
  - `UAPMD_ENABLE_LIBSONARE` : enables libsonare support (opt-in Apache2-licensed module)


## Release workflow

(Only for the release maintainers.)

- `grep -nRI {VERSION NUMBER HERE} | grep -v cmake-build-debug | grep -v external | grep -v js/node_modules | grep -v js/native` to find which file to replace hard coded version strings.
- git tag
- Once pushed, manually start release workflow on GitHub Actions.
  - It's better to only create a draft release, so that we can confirm that all the expected package files are there
