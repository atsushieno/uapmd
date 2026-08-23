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
// CHANGES: this file is a new implementation, written against the layer
// structure of upstream's exported model rather than translated from its
// source, but the network it reproduces and the constants it carries are
// upstream's. See HACKING.md in this directory.

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "OnnxWeights.hpp"

// The Basic Pitch network, written out directly rather than interpreted.
//
// The ONNX file is a weight store, not a graph: its 248 nodes are three
// quarters shape plumbing, and every shape is static because the input length
// is fixed. So the layers are spelled out here and the file is read only for
// the tensors they need. The layout below mirrors the exported graph exactly,
// including the octave ordering and pad amounts, because the reference
// activations it is checked against come from running that graph.
namespace uapmd_basic_pitch {

// Fixed geometry. The model was trained for exactly this window and cannot be
// asked for another.
inline constexpr int kSampleRate = 22050;
inline constexpr int kHopSamples = 256;
inline constexpr int kWindowSamples = kSampleRate * 2 - kHopSamples; // 43844
inline constexpr int kFrames = 172;
inline constexpr int kOctaves = 9;
inline constexpr int kBinsPerOctave = 36;   // 3 per semitone
inline constexpr int kCqtKernelTaps = 256;
inline constexpr int kRawCqtBins = kOctaves * kBinsPerOctave; // 324
inline constexpr int kCqtBins = 309;        // what survives the slice
inline constexpr int kHarmonics = 8;        // [0.5, 1, 2, 3, 4, 5, 6, 7]
inline constexpr int kStackedBins = 264;    // 88 semitones x 3
inline constexpr int kNoteBins = 88;

struct ModelWeights {
    // Two 36x256 kernel banks, applied per octave. Which one is the real part
    // and which the imaginary does not matter: only their magnitude is used,
    // so sign and order fall out of the arithmetic.
    std::vector<float> cqt_kernel_a;   // 36 x 256
    std::vector<float> cqt_kernel_b;   // 36 x 256
    std::vector<float> cqt_bias;       // 36, shared by both banks
    std::vector<float> lowpass;        // 256, the per-octave decimation filter
    float lowpass_bias{};
    std::vector<float> cqt_scale;      // 309, per-bin gain before the magnitude

    // The BatchNormalization that followed NormalizedLog, folded to an affine.
    float norm_scale{};
    float norm_shift{};

    struct Conv {
        std::vector<float> weight;
        std::vector<float> bias;
        int out_channels{}, in_channels{}, kernel_h{}, kernel_w{};
        int stride_h{1}, stride_w{1};
        int pad_top{}, pad_left{}, pad_bottom{}, pad_right{};
    };
    Conv contour_block;  // 8x8x3x39
    Conv contour_out;    // 1x8x5x5
    Conv note_stem;      // 32x1x7x7, stride (1,3)
    Conv note_out;       // 1x32x7x3
    Conv onset_stem;     // 32x8x5x5, stride (1,3)
    Conv onset_out;      // 1x33x3x3
};

// Binds the tensors this network needs out of an ONNX model. Every lookup is
// by name and checked against the expected shape; the build pins the model by
// SHA-256, so a name that no longer resolves means the pin was changed without
// this code being revisited, and must fail loudly rather than guess.
bool loadModelWeights(std::span<const uint8_t> onnx,
                      ModelWeights& weights,
                      std::string& error);

// Stage outputs, kept separately so each can be compared against the reference
// activations dumped from the original graph.
struct Activations {
    std::vector<float> cqt_magnitude;   // [bin][frame], 309 x 172
    std::vector<float> normalized_log;  // [frame][bin], 172 x 309
    std::vector<float> harmonic_stack;  // [harmonic][frame][bin], 8 x 172 x 264
    // The three posteriorgrams, each in [0, 1].
    std::vector<float> contour;         // [frame][bin], 172 x 264 (3 bins/semitone)
    std::vector<float> note;            // [frame][pitch], 172 x 88 (pitch 0 = A0)
    std::vector<float> onset;           // [frame][pitch], 172 x 88
};

// Runs the front end: nine-octave CQT, magnitude, log, min-max normalisation.
// `samples` must hold kWindowSamples values at kSampleRate.
void runFrontEnd(const ModelWeights& weights,
                 std::span<const float> samples,
                 Activations& activations);

// Aligns harmonics onto a common bin axis, then runs the six convolutions.
// Requires runFrontEnd() to have filled `normalized_log`.
void runNetwork(const ModelWeights& weights, Activations& activations);

// Front end plus network.
void run(const ModelWeights& weights,
         std::span<const float> samples,
         Activations& activations);

} // namespace uapmd_basic_pitch
