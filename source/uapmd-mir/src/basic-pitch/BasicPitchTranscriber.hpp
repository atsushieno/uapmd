// Derived from Basic Pitch <https://github.com/spotify/basic-pitch>.
//
// Copyright 2022 Spotify AB
// Copyright the UAPMD authors
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not
// use this file except in compliance with the License. You may obtain a copy
// of the License in the LICENSE file in this directory, or at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
// License for the specific language governing permissions and limitations
// under the License.
//
// CHANGES: this file is a C++ port of upstream's note_creation.py (note
// decoding, the melodia pass, inferred onsets and pitch bends) together with
// the windowing from inference.py. The algorithms and their thresholds follow
// upstream; the data structures, memory layout and cancellation are new. See
// HACKING.md in this directory.

#pragma once

#include <functional>
#include <span>
#include <vector>

#include "BasicPitchModel.hpp"

// Turning posteriorgrams into notes.
//
// The network only ever sees a fixed two-second window, so a whole clip is fed
// through as overlapping windows whose outputs are stitched back into one
// timeline. The note decoder that follows is a port of upstream's
// note_creation.py, kept faithful to it -- including the melodia pass and the
// inferred onsets -- because its thresholds were tuned against this model.
namespace uapmd_basic_pitch {

// Frames dropped from each end of a window before stitching, so that notes
// spanning a window boundary are decided by the window that saw the most
// context around them.
inline constexpr int kOverlapFrames = 30;
inline constexpr int kWindowHopSamples = kWindowSamples - kOverlapFrames * kHopSamples; // 36164
inline constexpr int kFramesPerWindow = kFrames - kOverlapFrames;                       // 142
inline constexpr int kMidiOffset = 21;   // pitch bin 0 is A0
inline constexpr int kMaxPitchIndex = kNoteBins - 1;

struct BasicPitchOptions {
    // Minimum onset activation for a peak to start a note.
    float onset_threshold{0.5f};
    // Minimum frame activation for a note to stay on.
    float frame_threshold{0.3f};
    double minimum_note_length_ms{127.7};
    // Add onsets wherever frame energy jumps, not just where the onset head
    // fired. Catches notes the onset head misses in legato passages.
    bool infer_onsets{true};
    // Second pass that harvests notes from frame energy no onset explained.
    bool melodia_trick{true};
    // Frames of sub-threshold energy tolerated before a note is ended.
    int energy_tolerance{11};
    // 0 disables the bound.
    double minimum_frequency{0.0};
    double maximum_frequency{0.0};
};

struct NoteEvent {
    int start_frame{};
    int end_frame{};
    int pitch_midi{};
    float amplitude{};
    // Per-frame deviation in thirds of a semitone, one entry per frame of the
    // note, read off the contour head. Empty when contours were unavailable.
    std::vector<int> bends;
};

// The stitched posteriorgrams for a whole signal.
struct Posteriorgrams {
    int frame_count{};
    std::vector<float> note;     // [frame][pitch], frame_count x 88
    std::vector<float> onset;    // [frame][pitch], frame_count x 88
    std::vector<float> contour;  // [frame][bin],   frame_count x 264
};

// Runs the network over `mono`, which must already be at kSampleRate.
// `progress` receives 0..1 and returns false to cancel; pass an empty function
// to run to completion. Returns false only if cancelled.
bool computePosteriorgrams(const ModelWeights& weights,
                           std::span<const float> mono,
                           Posteriorgrams& output,
                           const std::function<bool(double)>& progress = {});

std::vector<NoteEvent> decodeNotes(const Posteriorgrams& posteriorgrams,
                                   const BasicPitchOptions& options);

// Frame index to seconds, including the per-window alignment correction
// upstream applies.
double frameToSeconds(int frame);

} // namespace uapmd_basic_pitch
