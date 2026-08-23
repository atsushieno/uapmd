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

#include "BasicPitchModel.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <unordered_map>

namespace uapmd_basic_pitch {

namespace {

// Tensor names as exported. Ugly, but stable: the build pins the model file by
// SHA-256, so these cannot drift without the download failing first.
constexpr std::string_view kCqtKernelA = "const_fold_opt__664";
constexpr std::string_view kCqtKernelB = "const_fold_opt__655";
constexpr std::string_view kCqtBias =
    "model_1/cq_t2010v2_1/conv1d_25;model_1/cq_t2010v2_1/conv1d_25";
constexpr std::string_view kLowpass = "const_fold_opt__734";
constexpr std::string_view kLowpassBias =
    "model_1/conv2d_5/Conv2D;model_1/conv2d_5/Conv2D";
constexpr std::string_view kCqtScale =
    "model_1/cq_t2010v2_1/Sqrt;model_1/cq_t2010v2_1/Sqrt";

// The BatchNormalization between NormalizedLog and harmonic stacking, already
// folded to scale-and-shift by the exporter.
constexpr float kNormScale = 2.480741f;
constexpr float kNormShift = -0.87691832f;

using TensorMap = std::unordered_map<std::string_view, const OnnxTensor*>;

bool bind(const TensorMap& tensors, std::string_view name,
          std::vector<int64_t> expectedDims, std::vector<float>& out,
          std::string& error) {
    const auto found = tensors.find(name);
    if (found == tensors.end()) {
        error = std::format("Basic Pitch model has no tensor '{}'", name);
        return false;
    }
    if (found->second->dims != expectedDims) {
        error = std::format("Basic Pitch tensor '{}' has an unexpected shape", name);
        return false;
    }
    out = found->second->data;
    return true;
}

bool bindConv(const TensorMap& tensors, std::string_view weightName,
              std::string_view biasName, ModelWeights::Conv& conv,
              int outChannels, int inChannels, int kernelH, int kernelW,
              int strideH, int strideW,
              int padTop, int padLeft, int padBottom, int padRight,
              std::string& error) {
    if (!bind(tensors, weightName, {outChannels, inChannels, kernelH, kernelW},
              conv.weight, error))
        return false;
    if (!bind(tensors, biasName, {outChannels}, conv.bias, error))
        return false;
    conv.out_channels = outChannels;
    conv.in_channels = inChannels;
    conv.kernel_h = kernelH;
    conv.kernel_w = kernelW;
    conv.stride_h = strideH;
    conv.stride_w = strideW;
    conv.pad_top = padTop;
    conv.pad_left = padLeft;
    conv.pad_bottom = padBottom;
    conv.pad_right = padRight;
    return true;
}

// numpy/ONNX 'reflect': the edge sample is not repeated, so index -1 maps to 1.
int reflectIndex(int index, int length) {
    if (length == 1)
        return 0;
    const auto period = 2 * (length - 1);
    auto wrapped = index % period;
    if (wrapped < 0)
        wrapped += period;
    return wrapped < length ? wrapped : period - wrapped;
}

// One octave's CQT: 36 kernels of 256 taps stepped by `hop`, over a signal
// reflect-padded by half a kernel each side. Writes magnitudes straight out,
// because the two banks are only ever combined as sqrt(a^2 + b^2).
void octaveMagnitudes(const ModelWeights& weights,
                      std::span<const float> signal,
                      int hop,
                      float* destination,   // [bin][frame], stride kFrames
                      int destinationStride) {
    constexpr int taps = kCqtKernelTaps;
    constexpr int halfTaps = taps / 2;
    const auto length = static_cast<int>(signal.size());

    // Materialise the reflect-padded window once; the alternative is bounds
    // logic in the innermost loop, which is where all the time goes.
    std::vector<float> padded(static_cast<size_t>(length) + taps);
    for (int i = 0; i < length + taps; ++i)
        padded[static_cast<size_t>(i)] = signal[static_cast<size_t>(
            reflectIndex(i - halfTaps, length))];

    for (int bin = 0; bin < kBinsPerOctave; ++bin) {
        const float* kernelA = weights.cqt_kernel_a.data() + static_cast<size_t>(bin) * taps;
        const float* kernelB = weights.cqt_kernel_b.data() + static_cast<size_t>(bin) * taps;
        const auto bias = weights.cqt_bias[static_cast<size_t>(bin)];
        float* row = destination + static_cast<size_t>(bin) * destinationStride;
        for (int frame = 0; frame < kFrames; ++frame) {
            const float* window = padded.data() + static_cast<size_t>(frame) * hop;
            float a = bias;
            float b = bias;
            for (int tap = 0; tap < taps; ++tap) {
                a += window[tap] * kernelA[tap];
                b += window[tap] * kernelB[tap];
            }
            row[frame] = std::sqrt(a * a + b * b);
        }
    }
}

// The per-octave decimator: zero-pad by half a kernel, convolve, step by two.
std::vector<float> halveRate(const ModelWeights& weights, std::span<const float> signal) {
    constexpr int taps = kCqtKernelTaps;
    constexpr int pad = taps / 2 - 1; // 127, as the exported graph pads
    const auto length = static_cast<int>(signal.size());
    const auto paddedLength = length + 2 * pad;
    const auto outputLength = (paddedLength - taps) / 2 + 1;

    std::vector<float> padded(static_cast<size_t>(paddedLength), 0.0f);
    std::copy(signal.begin(), signal.end(),
              padded.begin() + pad);

    std::vector<float> output(static_cast<size_t>(outputLength));
    for (int i = 0; i < outputLength; ++i) {
        const float* window = padded.data() + static_cast<size_t>(i) * 2;
        float sum = weights.lowpass_bias;
        for (int tap = 0; tap < taps; ++tap)
            sum += window[tap] * weights.lowpass[static_cast<size_t>(tap)];
        output[static_cast<size_t>(i)] = sum;
    }
    return output;
}

} // namespace

bool loadModelWeights(std::span<const uint8_t> onnx,
                      ModelWeights& weights,
                      std::string& error) {
    std::vector<OnnxTensor> initializers;
    if (!readOnnxFloatInitializers(onnx, initializers, error))
        return false;

    TensorMap tensors;
    for (const auto& tensor : initializers)
        tensors.emplace(tensor.name, &tensor);

    if (!bind(tensors, kCqtKernelA, {kBinsPerOctave, 1, 1, kCqtKernelTaps},
              weights.cqt_kernel_a, error))
        return false;
    if (!bind(tensors, kCqtKernelB, {kBinsPerOctave, 1, 1, kCqtKernelTaps},
              weights.cqt_kernel_b, error))
        return false;
    if (!bind(tensors, kCqtBias, {kBinsPerOctave}, weights.cqt_bias, error))
        return false;
    if (!bind(tensors, kLowpass, {1, 1, 1, kCqtKernelTaps}, weights.lowpass, error))
        return false;
    std::vector<float> lowpassBias;
    if (!bind(tensors, kLowpassBias, {1}, lowpassBias, error))
        return false;
    weights.lowpass_bias = lowpassBias.front();
    if (!bind(tensors, kCqtScale, {kCqtBins, 1, 1}, weights.cqt_scale, error))
        return false;

    weights.norm_scale = kNormScale;
    weights.norm_shift = kNormShift;

    // Pads are ONNX order: [top, left, bottom, right].
    if (!bindConv(tensors, "const_fold_opt__727",
                  "model_1/re_lu_1/Relu;model_1/re_lu_1/Relu;model_1/batch_normalization_2/"
                  "FusedBatchNormV3;model_1/batch_normalization_2/FusedBatchNormV3;"
                  "model_1/conv2d_1/BiasAdd/ReadVariableOp;model_1/conv2d_1/BiasAdd/ReadVariableOp;"
                  "model_1/conv2d_1/BiasAdd;model_1/conv2d_1/BiasAdd;"
                  "model_1/conv2d_1/Conv2D;model_1/conv2d_1/Conv2D",
                  weights.contour_block, 8, 8, 3, 39, 1, 1, 1, 19, 1, 19, error))
        return false;
    if (!bindConv(tensors, "const_fold_opt__710",
                  "model_1/contours-reduced/BiasAdd/ReadVariableOp;"
                  "model_1/contours-reduced/BiasAdd/ReadVariableOp",
                  weights.contour_out, 1, 8, 5, 5, 1, 1, 2, 2, 2, 2, error))
        return false;
    if (!bindConv(tensors, "const_fold_opt__738",
                  "model_1/conv2d_2/BiasAdd/ReadVariableOp;model_1/conv2d_2/BiasAdd/ReadVariableOp",
                  weights.note_stem, 32, 1, 7, 7, 1, 3, 3, 2, 3, 2, error))
        return false;
    if (!bindConv(tensors, "const_fold_opt__702",
                  "model_1/conv2d_3/BiasAdd/ReadVariableOp;model_1/conv2d_3/BiasAdd/ReadVariableOp",
                  weights.note_out, 1, 32, 7, 3, 1, 1, 3, 1, 3, 1, error))
        return false;
    if (!bindConv(tensors, "const_fold_opt__707",
                  "model_1/re_lu_3/Relu;model_1/re_lu_3/Relu;model_1/batch_normalization_3/"
                  "FusedBatchNormV3;model_1/batch_normalization_3/FusedBatchNormV3;"
                  "model_1/conv2d_4/BiasAdd/ReadVariableOp;model_1/conv2d_4/BiasAdd/ReadVariableOp;"
                  "model_1/conv2d_4/BiasAdd;model_1/conv2d_4/BiasAdd;"
                  "model_1/conv2d_2/Conv2D;model_1/conv2d_2/Conv2D;"
                  "model_1/conv2d_4/Conv2D;model_1/conv2d_4/Conv2D",
                  weights.onset_stem, 32, 8, 5, 5, 1, 3, 2, 1, 2, 1, error))
        return false;
    if (!bindConv(tensors, "const_fold_opt__680",
                  "model_1/conv2d_5/BiasAdd/ReadVariableOp;model_1/conv2d_5/BiasAdd/ReadVariableOp",
                  weights.onset_out, 1, 33, 3, 3, 1, 1, 1, 1, 1, 1, error))
        return false;
    return true;
}

void runFrontEnd(const ModelWeights& weights,
                 std::span<const float> samples,
                 Activations& activations) {
    // Octave 0 runs at the input rate and holds the highest frequencies; each
    // decimation drops an octave. The exported graph concatenates them in
    // reverse, so octave 8 lands at bin 0 and the result reads low to high.
    std::vector<float> raw(static_cast<size_t>(kRawCqtBins) * kFrames);
    std::vector<float> signal(samples.begin(), samples.end());
    for (int octave = 0; octave < kOctaves; ++octave) {
        const auto binOffset = (kOctaves - 1 - octave) * kBinsPerOctave;
        octaveMagnitudes(weights, signal, kHopSamples >> octave,
                         raw.data() + static_cast<size_t>(binOffset) * kFrames, kFrames);
        if (octave + 1 < kOctaves)
            signal = halveRate(weights, signal);
    }

    // Keep the top 309 of 324 bins, scale each, and that is the magnitude the
    // reference calls cqt_magnitude.
    activations.cqt_magnitude.assign(static_cast<size_t>(kCqtBins) * kFrames, 0.0f);
    const auto droppedBins = kRawCqtBins - kCqtBins;
    for (int bin = 0; bin < kCqtBins; ++bin) {
        const auto scale = weights.cqt_scale[static_cast<size_t>(bin)];
        const float* source = raw.data() + static_cast<size_t>(bin + droppedBins) * kFrames;
        float* destination = activations.cqt_magnitude.data() + static_cast<size_t>(bin) * kFrames;
        for (int frame = 0; frame < kFrames; ++frame)
            destination[frame] = source[frame] * scale;
    }

    // 10*log10(magnitude^2 + 1e-10), then min-max to [0, 1] over the whole
    // window, with the degenerate all-equal case pinned to zero.
    activations.normalized_log.assign(static_cast<size_t>(kFrames) * kCqtBins, 0.0f);
    auto minimum = std::numeric_limits<float>::infinity();
    for (int bin = 0; bin < kCqtBins; ++bin) {
        for (int frame = 0; frame < kFrames; ++frame) {
            const auto magnitude = activations.cqt_magnitude[
                static_cast<size_t>(bin) * kFrames + frame];
            const auto decibels = 10.0f * std::log10(magnitude * magnitude + 1e-10f);
            activations.normalized_log[static_cast<size_t>(frame) * kCqtBins + bin] = decibels;
            minimum = std::min(minimum, decibels);
        }
    }
    auto maximum = 0.0f;
    for (auto& value : activations.normalized_log) {
        value -= minimum;
        maximum = std::max(maximum, value);
    }
    if (maximum == 0.0f)
        std::fill(activations.normalized_log.begin(), activations.normalized_log.end(), 0.0f);
    else
        for (auto& value : activations.normalized_log)
            value /= maximum;
}

namespace {

// Bin offsets for harmonics [0.5, 1, 2, 3, 4, 5, 6, 7], as round(36*log2(h)).
// The exported graph spells these out as slice/pad pairs; they reduce to
// out[bin] = in[bin + shift], zero outside the source range.
constexpr int kHarmonicShifts[kHarmonics] = {-36, 0, 36, 57, 72, 84, 93, 101};

// NCHW convolution with explicit ONNX-order padding. Deliberately the plain
// triple loop: this is checked against reference activations, so it is written
// to be obviously correct first, and the whole network is only ~0.5 GMAC per
// two-second window.
std::vector<float> convolve2d(const std::vector<float>& input,
                              int inputHeight, int inputWidth,
                              const ModelWeights::Conv& conv,
                              int& outputHeight, int& outputWidth) {
    outputHeight = (inputHeight + conv.pad_top + conv.pad_bottom - conv.kernel_h)
        / conv.stride_h + 1;
    outputWidth = (inputWidth + conv.pad_left + conv.pad_right - conv.kernel_w)
        / conv.stride_w + 1;

    std::vector<float> output(
        static_cast<size_t>(conv.out_channels) * outputHeight * outputWidth);

    for (int oc = 0; oc < conv.out_channels; ++oc) {
        for (int oy = 0; oy < outputHeight; ++oy) {
            const auto baseY = oy * conv.stride_h - conv.pad_top;
            for (int ox = 0; ox < outputWidth; ++ox) {
                const auto baseX = ox * conv.stride_w - conv.pad_left;
                float sum = conv.bias[static_cast<size_t>(oc)];
                for (int ic = 0; ic < conv.in_channels; ++ic) {
                    const float* plane = input.data()
                        + static_cast<size_t>(ic) * inputHeight * inputWidth;
                    const float* kernel = conv.weight.data()
                        + ((static_cast<size_t>(oc) * conv.in_channels + ic)
                           * conv.kernel_h) * conv.kernel_w;
                    for (int ky = 0; ky < conv.kernel_h; ++ky) {
                        const auto y = baseY + ky;
                        if (y < 0 || y >= inputHeight)
                            continue;
                        for (int kx = 0; kx < conv.kernel_w; ++kx) {
                            const auto x = baseX + kx;
                            if (x < 0 || x >= inputWidth)
                                continue;
                            sum += plane[static_cast<size_t>(y) * inputWidth + x]
                                * kernel[static_cast<size_t>(ky) * conv.kernel_w + kx];
                        }
                    }
                }
                output[(static_cast<size_t>(oc) * outputHeight + oy) * outputWidth + ox] = sum;
            }
        }
    }
    return output;
}

void applyRelu(std::vector<float>& values) {
    for (auto& value : values)
        value = value > 0.0f ? value : 0.0f;
}

void applySigmoid(std::vector<float>& values) {
    for (auto& value : values)
        value = 1.0f / (1.0f + std::exp(-value));
}

} // namespace

void runNetwork(const ModelWeights& weights, Activations& activations) {
    // Harmonic stacking. Each channel is the normalised CQT shifted so that a
    // given harmonic of a pitch lands on that pitch's bin, which is what lets a
    // small 2-D kernel see a whole harmonic series at once.
    activations.harmonic_stack.assign(
        static_cast<size_t>(kHarmonics) * kFrames * kStackedBins, 0.0f);
    for (int harmonic = 0; harmonic < kHarmonics; ++harmonic) {
        const auto shift = kHarmonicShifts[harmonic];
        float* plane = activations.harmonic_stack.data()
            + static_cast<size_t>(harmonic) * kFrames * kStackedBins;
        for (int frame = 0; frame < kFrames; ++frame) {
            const float* source = activations.normalized_log.data()
                + static_cast<size_t>(frame) * kCqtBins;
            float* destination = plane + static_cast<size_t>(frame) * kStackedBins;
            for (int bin = 0; bin < kStackedBins; ++bin) {
                const auto sourceBin = bin + shift;
                destination[bin] = (sourceBin >= 0 && sourceBin < kCqtBins)
                    ? source[sourceBin] * weights.norm_scale + weights.norm_shift
                    : 0.0f;
            }
        }
    }

    int height = 0;
    int width = 0;

    // Contour head: one 3x39 block over the stack, then a 5x5 reduction.
    auto contour = convolve2d(activations.harmonic_stack, kFrames, kStackedBins,
                              weights.contour_block, height, width);
    applyRelu(contour);
    contour = convolve2d(contour, height, width, weights.contour_out, height, width);
    applySigmoid(contour);
    activations.contour = contour;

    // Note head reads the contour posteriorgram, narrowing 3 bins per semitone
    // down to 1 with a stride-3 convolution.
    auto note = convolve2d(activations.contour, kFrames, kStackedBins,
                           weights.note_stem, height, width);
    applyRelu(note);
    auto noteStem = note;
    const auto noteStemHeight = height;
    const auto noteStemWidth = width;
    note = convolve2d(note, height, width, weights.note_out, height, width);
    applySigmoid(note);
    activations.note = note;

    // Onset head runs its own stem straight off the stack, then sees the note
    // posteriorgram alongside it: an onset is a note starting, so the two heads
    // are deliberately not independent.
    auto onsetStem = convolve2d(activations.harmonic_stack, kFrames, kStackedBins,
                                weights.onset_stem, height, width);
    applyRelu(onsetStem);

    std::vector<float> combined;
    combined.reserve(activations.note.size() + onsetStem.size());
    combined.insert(combined.end(), activations.note.begin(), activations.note.end());
    combined.insert(combined.end(), onsetStem.begin(), onsetStem.end());

    auto onset = convolve2d(combined, height, width, weights.onset_out, height, width);
    applySigmoid(onset);
    activations.onset = onset;

    (void) noteStem;
    (void) noteStemHeight;
    (void) noteStemWidth;
}

void run(const ModelWeights& weights,
         std::span<const float> samples,
         Activations& activations) {
    runFrontEnd(weights, samples, activations);
    runNetwork(weights, activations);
}

} // namespace uapmd_basic_pitch
