#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace uapmd_stems {

// Deinterleaved stereo at a single sample rate. Both separation backends want
// their input in exactly this shape, differing only in the rate their model
// was trained at.
struct StereoAudio {
    std::vector<float> left;
    std::vector<float> right;

    size_t frameCount() const { return std::min(left.size(), right.size()); }
};

// Reads any format uapmd-data can open, downmixes to stereo (odd channels feed
// the left, even ones the right) and resamples to `targetSampleRate`.
// Returns false and fills `error` when the file cannot be used.
bool loadStereoAudio(const std::string& filepath,
                     uint32_t targetSampleRate,
                     StereoAudio& audio,
                     std::string& error);

// Writes a stereo WAV. `left` and `right` must both hold `frameCount` samples.
bool writeStereoWav(const std::filesystem::path& path,
                    const float* left,
                    const float* right,
                    size_t frameCount,
                    uint32_t sampleRate);

} // namespace uapmd_stems
