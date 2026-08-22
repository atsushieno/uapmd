
# Developers Guide

## Project Structure & Module Organization

In the latest source tree, the following description applies to the `source` directory.

- `remidy/`: plugin API abstraction (VST3/AU/LV2/CLAP backends).
- `remidy-gui`: common GUI helper that is required by plugin formats (such as `ContainerWindow`)
- `uapmd-plugin-hosting/`: commonized scanning/instancing utilities, and the plugin hosting API (`AudioPluginHostingAPI` / `AudioPluginInstanceAPI`) plus its `remidy`-backed implementation.
- `uapmd-midi-service/`: virtual MIDI 2.0 device foundation (function block manager, UMP mapping).
- `uapmd-graph/`: audio graph and graph nodes.
- `uapmd-data/`: sequencer data structures.
- `uapmd-file/`: platform abstraction API for file dialog and file system.
- `uapmd-engine/`: sequencer engine.
- `uapmd-mir/`: the opt-in music-analysis addins (`UAPMD_ENABLE_MIR`, off by default, backed by librosa.cpp and
  libsonare), plus the stem separation addins backing audio import -- demucs.cpp and BSRoformer.cpp -- which are
  always built. The separation backends share `src/StemSeparationSupport.*` for audio loading, resampling and stem writing.
- `tools/`: tools
  - `uapmd-app`: an example DAW-like sequencer that also serves virtual UMP devices, for dogfooding
  - `uapmd-app-model`: model API for uapmd-app, to be shared with C API and bindings
  - `uapmd-scan`: standalone plugin scanner (same engine as the app's `--scan-only` mode)
  - `uapmd-apply`: console tool to statically renderer UAPMD project to audio file.
  - `remidy-gui-shared`: some uapmd-specific shared code to implement GUI features (it used to be shared between multiple apps, but not nowaday)
- `external/`: Third‑party dependencies (submodules/FetchContent).
- `cmake/`: CMake build helpers

## Release workflow

(Only for the release maintainers.)

- `grep -nRI {VERSION NUMBER HERE} | grep -v cmake-build-debug | grep -v external | grep -v js/node_modules | grep -v js/native` to find which file to replace hard coded version strings.
- git tag
- Once pushed, manually start release workflow on GitHub Actions.
  - It's better to only create a draft release, so that we can confirm that all the expected package files are there
