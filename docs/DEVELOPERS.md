
# Developers Guide

## Project Structure & Module Organization

- `include/`: the library public API (`include/{module}`)
  - They are split under `include/{module}/priv` (direct references to those files may break at any version)
- `src/`: the library sources.
  - `src/remidy/`: plugin API abstraction (VST3/AU/LV2/CLAP backends).
  - `src/remidy-tooling/`: commonized scanning/instancing utilities.
  - `src/uapmd/`: virtual MIDI 2.0 device foundation (function block manager, plugin hosting API integration).
  - `src/uapmd-data/`: sequencer data structures.
  - `src/uapmd-engine/`: sequencer engine.
- `tools/`: tools
  - `uapmd-app`: an example plugin host that also serves virtual UMP devices
  - `remidy-scan`: standalone plugin scanner
- `external/`: Third‑party dependencies (submodules/FetchContent).
- `cmake/`: CMake build helpers

## Release workflow

(Only for the release maintainers.)

- `grep -nRI {VERSION NUMBER HERE} | grep -v cmake-build-debug | grep -v external | grep -v js/node_modules | grep -v js/native` to find which file to replace hard coded version strings.
- git tag
- Once pushed, manually start release workflow on GitHub Actions.
  - It's better to only create a draft release, so that we can confirm that all the expected package files are there
