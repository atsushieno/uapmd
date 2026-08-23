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

#include "BasicPitchTranscriber.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace uapmd_basic_pitch {

namespace {

constexpr double kAnnotationsBaseFrequency = 27.5; // A0
constexpr int kContoursBinsPerSemitone = 3;
constexpr int kBendBinsTolerance = 25;
constexpr double kBendGaussianStd = 5.0;
// Upstream carries this constant to line the frame grid up with the audio.
constexpr double kMagicAlignmentOffset = 0.0018;

double midiToHz(double midi) {
    return 440.0 * std::pow(2.0, (midi - 69.0) / 12.0);
}

double midiPitchToContourBin(double midi) {
    return 12.0 * kContoursBinsPerSemitone
        * std::log2(midiToHz(midi) / kAnnotationsBaseFrequency);
}

// Onsets wherever frame energy rises sharply, merged with the onset head's own
// prediction. Mirrors get_infered_onsets() with n_diff = 2.
void addInferredOnsets(const std::vector<float>& frames,
                       std::vector<float>& onsets,
                       int frameCount) {
    constexpr int kDiffs = 2;
    std::vector<float> difference(frames.size(), 0.0f);
    for (int frame = 0; frame < frameCount; ++frame) {
        for (int pitch = 0; pitch < kNoteBins; ++pitch) {
            const auto index = static_cast<size_t>(frame) * kNoteBins + pitch;
            auto smallest = std::numeric_limits<float>::infinity();
            for (int lag = 1; lag <= kDiffs; ++lag) {
                const auto previous = frame >= lag
                    ? frames[static_cast<size_t>(frame - lag) * kNoteBins + pitch]
                    : 0.0f;
                smallest = std::min(smallest, frames[index] - previous);
            }
            difference[index] = frame < kDiffs ? 0.0f : std::max(0.0f, smallest);
        }
    }
    const auto onsetMax = *std::ranges::max_element(onsets);
    const auto differenceMax = *std::ranges::max_element(difference);
    if (differenceMax <= 0.0f)
        return;
    const auto rescale = onsetMax / differenceMax;
    for (size_t i = 0; i < onsets.size(); ++i)
        onsets[i] = std::max(onsets[i], difference[i] * rescale);
}

void constrainFrequency(std::vector<float>& onsets, std::vector<float>& frames,
                        int frameCount, const BasicPitchOptions& options) {
    const auto toIndex = [](double hz) {
        return static_cast<int>(std::lround(
            69.0 + 12.0 * std::log2(hz / 440.0) - kMidiOffset));
    };
    const auto lowest = options.minimum_frequency > 0.0
        ? std::clamp(toIndex(options.minimum_frequency), 0, kNoteBins) : 0;
    const auto highest = options.maximum_frequency > 0.0
        ? std::clamp(toIndex(options.maximum_frequency), 0, kNoteBins) : kNoteBins;
    if (lowest == 0 && highest == kNoteBins)
        return;
    for (int frame = 0; frame < frameCount; ++frame) {
        for (int pitch = 0; pitch < kNoteBins; ++pitch) {
            if (pitch >= lowest && pitch < highest)
                continue;
            const auto index = static_cast<size_t>(frame) * kNoteBins + pitch;
            onsets[index] = 0.0f;
            frames[index] = 0.0f;
        }
    }
}

// Zeroes a note's own band plus its neighbours, so the next onset cannot claim
// the same energy one bin away.
void clearBand(std::vector<float>& energy, int fromFrame, int toFrame, int pitch) {
    for (int frame = fromFrame; frame < toFrame; ++frame) {
        for (int neighbour = pitch - 1; neighbour <= pitch + 1; ++neighbour) {
            if (neighbour < 0 || neighbour > kMaxPitchIndex)
                continue;
            energy[static_cast<size_t>(frame) * kNoteBins + neighbour] = 0.0f;
        }
    }
}

float meanEnergy(const std::vector<float>& frames, int fromFrame, int toFrame, int pitch) {
    if (toFrame <= fromFrame)
        return 0.0f;
    double sum = 0.0;
    for (int frame = fromFrame; frame < toFrame; ++frame)
        sum += frames[static_cast<size_t>(frame) * kNoteBins + pitch];
    return static_cast<float>(sum / (toFrame - fromFrame));
}

} // namespace

double frameToSeconds(int frame) {
    const auto plain = static_cast<double>(frame) * kHopSamples / kSampleRate;
    // Each window contributes slightly less audio than its frame count implies,
    // so the grid drifts by a fixed amount per window.
    const auto windowNumber = std::floor(static_cast<double>(frame) / kFrames);
    const auto windowOffset =
        (static_cast<double>(kHopSamples) / kSampleRate)
            * (kFrames - static_cast<double>(kWindowSamples) / kHopSamples)
        + kMagicAlignmentOffset;
    return plain - windowOffset * windowNumber;
}

bool computePosteriorgrams(const ModelWeights& weights,
                           std::span<const float> mono,
                           Posteriorgrams& output,
                           const std::function<bool(double)>& progress) {
    // Half the overlap is prepended so the first real sample sits inside the
    // part of window zero that survives trimming.
    const auto leadIn = kOverlapFrames * kHopSamples / 2;
    std::vector<float> padded(static_cast<size_t>(leadIn), 0.0f);
    padded.insert(padded.end(), mono.begin(), mono.end());

    const auto originalLength = static_cast<int64_t>(mono.size());
    const auto windowCount = std::max<int64_t>(
        1, (static_cast<int64_t>(padded.size()) + kWindowHopSamples - 1) / kWindowHopSamples);

    output.note.clear();
    output.onset.clear();
    output.contour.clear();
    output.note.reserve(static_cast<size_t>(windowCount) * kFramesPerWindow * kNoteBins);
    output.onset.reserve(static_cast<size_t>(windowCount) * kFramesPerWindow * kNoteBins);
    output.contour.reserve(static_cast<size_t>(windowCount) * kFramesPerWindow * kStackedBins);

    std::vector<float> window(static_cast<size_t>(kWindowSamples));
    Activations activations;
    const auto trim = kOverlapFrames / 2;

    for (int64_t index = 0; index < windowCount; ++index) {
        if (progress && !progress(static_cast<double>(index) / static_cast<double>(windowCount)))
            return false;

        const auto offset = index * kWindowHopSamples;
        std::fill(window.begin(), window.end(), 0.0f);
        const auto available = std::min<int64_t>(
            kWindowSamples, static_cast<int64_t>(padded.size()) - offset);
        if (available > 0)
            std::copy_n(padded.begin() + offset, available, window.begin());

        run(weights, window, activations);

        // Drop half the overlap from each end; what remains abuts the next
        // window exactly.
        for (int frame = trim; frame < kFrames - trim; ++frame) {
            const auto* noteRow = activations.note.data() + static_cast<size_t>(frame) * kNoteBins;
            const auto* onsetRow = activations.onset.data() + static_cast<size_t>(frame) * kNoteBins;
            const auto* contourRow = activations.contour.data() + static_cast<size_t>(frame) * kStackedBins;
            output.note.insert(output.note.end(), noteRow, noteRow + kNoteBins);
            output.onset.insert(output.onset.end(), onsetRow, onsetRow + kNoteBins);
            output.contour.insert(output.contour.end(), contourRow, contourRow + kStackedBins);
        }
    }

    // Trim the tail that only existed because the last window had to be filled.
    const auto expected = static_cast<int64_t>(
        (static_cast<double>(originalLength) / kWindowHopSamples) * kFramesPerWindow);
    const auto produced = static_cast<int64_t>(output.note.size() / kNoteBins);
    output.frame_count = static_cast<int>(std::clamp<int64_t>(expected, 0, produced));
    output.note.resize(static_cast<size_t>(output.frame_count) * kNoteBins);
    output.onset.resize(static_cast<size_t>(output.frame_count) * kNoteBins);
    output.contour.resize(static_cast<size_t>(output.frame_count) * kStackedBins);
    return true;
}

std::vector<NoteEvent> decodeNotes(const Posteriorgrams& posteriorgrams,
                                   const BasicPitchOptions& options) {
    std::vector<NoteEvent> events;
    const auto frameCount = posteriorgrams.frame_count;
    if (frameCount <= 1)
        return events;

    auto frames = posteriorgrams.note;
    auto onsets = posteriorgrams.onset;
    constrainFrequency(onsets, frames, frameCount, options);
    if (options.infer_onsets)
        addInferredOnsets(frames, onsets, frameCount);

    const auto minimumLength = static_cast<int>(std::lround(
        options.minimum_note_length_ms / 1000.0
        * (static_cast<double>(kSampleRate) / kHopSamples)));
    const auto tolerance = options.energy_tolerance;

    auto remaining = frames;
    const auto at = [&](const std::vector<float>& matrix, int frame, int pitch) {
        return matrix[static_cast<size_t>(frame) * kNoteBins + pitch];
    };

    // Onsets are strict local maxima in time, walked backwards so that when two
    // onsets compete for the same energy the later one -- the one whose note is
    // still sounding -- is not truncated by the earlier.
    std::vector<std::pair<int, int>> peaks;
    for (int frame = 1; frame < frameCount - 1; ++frame)
        for (int pitch = 0; pitch < kNoteBins; ++pitch)
            if (at(onsets, frame, pitch) > at(onsets, frame - 1, pitch)
                && at(onsets, frame, pitch) > at(onsets, frame + 1, pitch)
                && at(onsets, frame, pitch) >= options.onset_threshold)
                peaks.emplace_back(frame, pitch);
    std::ranges::reverse(peaks);

    for (const auto& [startFrame, pitch] : peaks) {
        if (startFrame >= frameCount - 1)
            continue;
        int frame = startFrame + 1;
        int quiet = 0;
        while (frame < frameCount - 1 && quiet < tolerance) {
            quiet = at(remaining, frame, pitch) < options.frame_threshold ? quiet + 1 : 0;
            ++frame;
        }
        frame -= quiet; // back up to the last frame that was still sounding
        if (frame - startFrame <= minimumLength)
            continue;
        clearBand(remaining, startFrame, frame, pitch);
        events.push_back({startFrame, frame, pitch + kMidiOffset,
                          meanEnergy(frames, startFrame, frame, pitch), {}});
    }

    // Melodia: whatever frame energy no onset explained is still a note, it just
    // began too gently for the onset head to see. Grow each remaining peak in
    // both directions and consume it.
    if (options.melodia_trick) {
        while (true) {
            auto best = 0.0f;
            int bestFrame = -1;
            int bestPitch = -1;
            for (int f = 0; f < frameCount; ++f) {
                for (int p = 0; p < kNoteBins; ++p) {
                    const auto value = at(remaining, f, p);
                    if (value > best) {
                        best = value;
                        bestFrame = f;
                        bestPitch = p;
                    }
                }
            }
            if (bestFrame < 0 || best <= options.frame_threshold)
                break;
            remaining[static_cast<size_t>(bestFrame) * kNoteBins + bestPitch] = 0.0f;

            int frame = bestFrame + 1;
            int quiet = 0;
            while (frame < frameCount - 1 && quiet < tolerance) {
                quiet = at(remaining, frame, bestPitch) < options.frame_threshold ? quiet + 1 : 0;
                clearBand(remaining, frame, frame + 1, bestPitch);
                ++frame;
            }
            const auto endFrame = frame - 1 - quiet;

            frame = bestFrame - 1;
            quiet = 0;
            while (frame > 0 && quiet < tolerance) {
                quiet = at(remaining, frame, bestPitch) < options.frame_threshold ? quiet + 1 : 0;
                clearBand(remaining, frame, frame + 1, bestPitch);
                --frame;
            }
            const auto beginFrame = frame + 1 + quiet;

            if (endFrame - beginFrame <= minimumLength)
                continue;
            events.push_back({beginFrame, endFrame, bestPitch + kMidiOffset,
                              meanEnergy(frames, beginFrame, endFrame, bestPitch), {}});
        }
    }

    // Pitch bend per note: within a window around the note's expected contour
    // bin, weighted so distant bins cannot win, take the strongest bin each
    // frame. The offset from the expected bin is the bend, in thirds of a
    // semitone.
    if (!posteriorgrams.contour.empty()) {
        constexpr int windowLength = kBendBinsTolerance * 2 + 1;
        std::vector<float> gaussian(windowLength);
        for (int i = 0; i < windowLength; ++i) {
            const auto x = (i - kBendBinsTolerance) / kBendGaussianStd;
            gaussian[static_cast<size_t>(i)] = static_cast<float>(std::exp(-0.5 * x * x));
        }
        for (auto& event : events) {
            const auto centre = static_cast<int>(std::lround(
                midiPitchToContourBin(event.pitch_midi)));
            const auto first = std::max(centre - kBendBinsTolerance, 0);
            const auto last = std::min(kStackedBins, centre + kBendBinsTolerance + 1);
            if (first >= last)
                continue;
            const auto shift = kBendBinsTolerance - std::max(0, kBendBinsTolerance - centre);
            event.bends.reserve(static_cast<size_t>(event.end_frame - event.start_frame));
            for (int frame = event.start_frame; frame < event.end_frame; ++frame) {
                auto best = -std::numeric_limits<float>::infinity();
                int bestOffset = 0;
                for (int bin = first; bin < last; ++bin) {
                    const auto weight = gaussian[static_cast<size_t>(
                        bin - centre + kBendBinsTolerance)];
                    const auto value = posteriorgrams.contour[
                        static_cast<size_t>(frame) * kStackedBins + bin] * weight;
                    if (value > best) {
                        best = value;
                        bestOffset = bin - first;
                    }
                }
                event.bends.push_back(bestOffset - shift);
            }
        }
    }

    std::ranges::sort(events, [](const NoteEvent& left, const NoteEvent& right) {
        if (left.start_frame != right.start_frame)
            return left.start_frame < right.start_frame;
        return left.pitch_midi < right.pitch_midi;
    });
    return events;
}

} // namespace uapmd_basic_pitch
