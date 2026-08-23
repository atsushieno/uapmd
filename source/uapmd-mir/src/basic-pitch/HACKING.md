# Hacking: Basic Pitch

Work in this directory derives from
[spotify/basic-pitch](https://github.com/spotify/basic-pitch), Apache-2.0,
Copyright 2022 Spotify AB. `LICENSE` carries the licence text and `NOTICE`
reproduces upstream's attributions, as sections 4(a) and 4(d) require.

## The model weights

The weights are **not committed**, but they *are* redistributed in any binary
built with `UAPMD_ENABLE_BASIC_PITCH`, so their origin matters as much as the
source files'.

| | |
|---|---|
| artifact | `nmp.onnx` |
| source | `https://raw.githubusercontent.com/spotify/basic-pitch/main/basic_pitch/saved_models/icassp_2022/nmp.onnx` |
| pinned by | SHA-256 `2c3c1d144bfa61ad236e92e169c13535c880469a12a047d4e73451f2c059a0ec` |
| size | 230,444 bytes |
| licence | Apache-2.0, Copyright 2022 Spotify AB |
| fetched | at configure time by `../../CMakeLists.txt`, into the build directory |
| embedded as | `basic_pitch_model_data.cpp`, **generated**, hex encoded, never committed |

`main` is a moving branch, so the hash rather than the tag is what pins the
model: a silent upstream retrain must fail the build rather than change what
ships. The generated file carries the same attribution in its own header,
because that file — not `nmp.onnx` — is what the compiler and the shipped
binary actually see.

What the feature does, and how accurate it is, is documented in
`docs/uapmd-mir/BASIC_PITCH.md`; this file is only about where the code came
from.

## What each file is

| file | origin |
|---|---|
| `BasicPitchTranscriber.{hpp,cpp}` | **Port of upstream Python.** `decodeNotes()` translates `note_creation.py` -- `output_to_notes_polyphonic`, `get_infered_onsets`, `constrain_frequency`, `get_pitch_bends`, `midi_pitch_to_contour_bin` -- and `computePosteriorgrams()` translates the windowing in `inference.py` (`window_audio_file`, `get_audio_input`, `unwrap_output`). `frameToSeconds()` is `model_frames_to_time`. Algorithms and thresholds are upstream's; the data structures, memory layout and cancellation support are new. |
| `BasicPitchModel.{hpp,cpp}` | **New implementation of upstream's network.** Written against the layer structure of the exported `nmp.onnx` -- shapes, strides, pads, concat order read out of the graph -- rather than translated from `models.py` or `nn.py`. The architecture it reproduces, and the constants it carries (harmonic shifts, the folded BatchNormalization, tensor names), are upstream's. |
| `OnnxWeights.{hpp,cpp}` | Original. A protobuf wire-format reader for four ONNX fields. ONNX is an open specification; no upstream code was consulted. |
| `EmbeddedModel.cpp` | Original. Decodes the hex blob CMake generates. |
| `BasicPitchAddin.cpp` | Original. Mirrors this project's own `PitchTranscriptionAddin.cpp`. |
| `basic_pitch_model_data.cpp` | **Generated, not committed, not in this directory.** Written into the build directory at configure time from the downloaded `nmp.onnx`, whose bytes it carries verbatim as hex. It is therefore Apache-2.0 material, and the largest single piece of it in the build: everything else here is code, this is the model itself. See the section above. |

## Why it is a port rather than a reimplementation

The note decoder's thresholds were tuned against this specific model, and two of
its stages -- the inferred onsets and the melodia pass -- recover notes a plainly
reasonable decoder would miss. Staying faithful also makes the port checkable
against upstream's own decoder on identical posteriorgrams, which is how it was
validated.

The consequence is that `BasicPitchTranscriber.{hpp,cpp}` is an Apache-2.0
derivative work and stays one regardless of whether `UAPMD_ENABLE_BASIC_PITCH`
is set, because unlike the weights it is committed source. That is consistent
with the top-level README, which already releases the whole `uapmd-mir` module
under Apache V2, but it is a deliberate decision rather than an accident.
