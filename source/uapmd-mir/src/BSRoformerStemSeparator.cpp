#include "BSRoformerStemSeparator.hpp"

#include "StemSeparationSupport.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <format>
#include <string>
#include <system_error>
#include <vector>

#include <bs_roformer/inference.h>
#include <remidy/remidy.hpp>

namespace uapmd_bsroformer {

using uapmd::import::StemFile;
using uapmd::import::StemSeparationRequest;
using uapmd::import::StemSeparationResult;
using uapmd::import::StemSeparatorModelFileSpec;

namespace {

constexpr float kProgressAfterLoad = 0.05f;
constexpr float kProgressAfterSeparation = 0.90f;

// The name given to the complement derived for a single-stem model.
constexpr std::string_view kResidualStemName{"instrumental"};

// The GGUF format carries a stem *count* and no stem names, so these have to
// be inferred. Published BS-Roformer and Mel-Band-Roformer models are vocal
// separators whose first stem is the vocal -- BSRoformer.cpp's own CLI writes
// stem 0 to the output path the user names "vocals.wav". Anything wider than
// the vocal/instrumental case is numbered rather than guessed at.
std::vector<std::string> stemNames(size_t stemCount) {
    if (stemCount == 1)
        return {"vocals"};
    if (stemCount == 2)
        return {"vocals", "instrumental"};

    std::vector<std::string> names;
    names.reserve(stemCount);
    for (size_t index = 0; index < stemCount; ++index)
        names.push_back(std::format("stem{}", index + 1));
    return names;
}

std::string formatDuration(double seconds) {
    if (seconds < 1.0)
        return "moments";
    if (seconds < 90.0)
        return std::format("{}s", static_cast<int>(std::lround(seconds)));
    return std::format("{}m", static_cast<int>(std::lround(seconds / 60.0)));
}

// Mirrors the padding and stepping ProcessOverlapAdd() applies internally, so
// the run can say up front how many inference passes it is committing to.
int expectedChunkCount(size_t frameCount, int chunkSize, int overlap) {
    const int step = chunkSize / overlap;
    if (step <= 0)
        return 0;
    const int border = chunkSize - step;
    const auto frames = static_cast<int64_t>(frameCount);
    const bool padded = frames > 2 * border && border > 0;
    const auto paddedFrames = padded ? frames + 2 * border : frames;
    return static_cast<int>((paddedFrames + step - 1) / step);
}

// One stem ready to be written: planar stereo, named.
struct PreparedStem {
    std::string name;
    std::vector<float> left;
    std::vector<float> right;
};

std::vector<float> interleaveStereo(const uapmd_stems::StereoAudio& audio) {
    const size_t frameCount = audio.frameCount();
    std::vector<float> interleaved(frameCount * 2);
    for (size_t frame = 0; frame < frameCount; ++frame) {
        interleaved[frame * 2] = audio.left[frame];
        interleaved[frame * 2 + 1] = audio.right[frame];
    }
    return interleaved;
}

} // namespace

std::string_view BSRoformerStemSeparator::id() const noexcept {
    return "bs-roformer";
}

std::string_view BSRoformerStemSeparator::name() const noexcept {
    return "BS-Roformer";
}

StemSeparatorModelFileSpec BSRoformerStemSeparator::modelFileSpec() const {
    return {"BS-Roformer GGUF Model", {"*.gguf"}};
}

StemSeparationResult BSRoformerStemSeparator::separate(const StemSeparationRequest& request) const
{
    StemSeparationResult result;
    if (request.modelPath.empty()) {
        result.error = "BS-Roformer model path is empty";
        return result;
    }

    // Inference reports cancellation by throwing, so remember that we asked
    // for it and tell the two kinds of cancellation apart in the handler.
    bool cancelRequested = false;
    auto shouldCancel = [&]() {
        if (cancelRequested)
            return true;
        if (request.shouldCancel && request.shouldCancel())
            cancelRequested = true;
        return cancelRequested;
    };
    auto emitProgress = [&](float value, const std::string& message) {
        if (request.progressCallback && !request.progressCallback(value, message))
            cancelRequested = true;
    };

    try {
        if (shouldCancel()) {
            result.canceled = true;
            return result;
        }

        emitProgress(0.0f, "Loading BS-Roformer model...");
        Inference inference(request.modelPath);

        const auto modelSampleRate = inference.GetSampleRate();
        if (modelSampleRate <= 0) {
            result.error = "BS-Roformer model reports an invalid sample rate";
            return result;
        }
        const auto sampleRate = static_cast<uint32_t>(modelSampleRate);

        emitProgress(kProgressAfterLoad, "Preparing BS-Roformer input...");
        uapmd_stems::StereoAudio input;
        if (!uapmd_stems::loadStereoAudio(request.audioFile, sampleRate, input, result.error))
            return result;

        if (shouldCancel()) {
            result.canceled = true;
            return result;
        }

        const auto interleaved = interleaveStereo(input);

        // These come from the model, and they decide how long the run takes:
        // every sample is processed `overlap` times, so the total work is
        // roughly (audio duration / step) inference passes. Say so before
        // starting -- progress only arrives once a whole pass completes, and
        // one pass is tens of seconds on CPU.
        const auto chunkSize = inference.GetDefaultChunkSize();
        const auto overlap = std::max(1, inference.GetDefaultNumOverlap());
        const auto inputSeconds = static_cast<double>(input.frameCount()) / sampleRate;
        const auto totalChunks = expectedChunkCount(input.frameCount(), chunkSize, overlap);
        remidy::Logger::global()->logInfo(
            "BS-Roformer: %.1fs of audio; chunk %.1fs, overlap %d -> %d inference passes",
            inputSeconds, static_cast<double>(chunkSize) / sampleRate, overlap, totalChunks);

        const auto separationStart = std::chrono::steady_clock::now();
        emitProgress(kProgressAfterLoad,
                     std::format("Separating stems (pass 1 of {})...", std::max(totalChunks, 1)));

        const auto stems = inference.Process(
            interleaved,
            chunkSize,
            overlap,
            [&](float progress) {
                const auto fraction = std::clamp(progress, 0.0f, 1.0f);
                const auto elapsed = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - separationStart).count();
                const auto done = std::max(1, static_cast<int>(std::lround(fraction * totalChunks)));
                const auto remaining = fraction > 0.0f ? elapsed * (1.0 - fraction) / fraction : 0.0;

                remidy::Logger::global()->logInfo(
                    "BS-Roformer: pass %d/%d (%.0f%%), %.0fs elapsed, ~%.0fs left",
                    done, totalChunks, fraction * 100.0, elapsed, remaining);

                const auto scaled = kProgressAfterLoad
                    + fraction * (kProgressAfterSeparation - kProgressAfterLoad);
                emitProgress(scaled, std::format("Separating stems: pass {} of {}, ~{} left",
                                                 done, std::max(totalChunks, 1),
                                                 formatDuration(remaining)));
            },
            [&]() { return shouldCancel(); });

        if (shouldCancel()) {
            result.canceled = true;
            return result;
        }
        if (stems.empty()) {
            result.error = "BS-Roformer returned no stems";
            return result;
        }

        std::error_code ec;
        std::filesystem::create_directories(request.outputDirectory, ec);
        if (ec) {
            result.error = std::format("Failed to create output directory: {}", ec.message());
            return result;
        }

        const auto labels = stemNames(stems.size());
        const auto baseName = std::filesystem::path(request.audioFile).stem().string();

        // Process() hands back interleaved stereo; the writer wants planar.
        std::vector<PreparedStem> prepared;
        prepared.reserve(stems.size() + 1);
        for (size_t index = 0; index < stems.size(); ++index) {
            const auto& stem = stems[index];
            const size_t frameCount = stem.size() / 2;
            PreparedStem entry;
            entry.name = labels[index];
            entry.left.resize(frameCount);
            entry.right.resize(frameCount);
            for (size_t frame = 0; frame < frameCount; ++frame) {
                entry.left[frame] = stem[frame * 2];
                entry.right[frame] = stem[frame * 2 + 1];
            }
            prepared.push_back(std::move(entry));
        }

        // Most published models are target extractors reporting num_stems=1:
        // they emit the vocal only, which would import as a single track where
        // Demucs gives four. The complement is what the rest of the mix is, so
        // derive it by subtracting the estimate from the input the model saw.
        // Process() crops its output back to the input length, so the two are
        // already sample-aligned.
        if (prepared.size() == 1) {
            const auto& target = prepared.front();
            const size_t frameCount = std::min(target.left.size(), input.frameCount());
            PreparedStem residual;
            residual.name = std::string(kResidualStemName);
            residual.left.resize(frameCount);
            residual.right.resize(frameCount);
            for (size_t frame = 0; frame < frameCount; ++frame) {
                residual.left[frame] = input.left[frame] - target.left[frame];
                residual.right[frame] = input.right[frame] - target.right[frame];
            }
            remidy::Logger::global()->logInfo(
                "BS-Roformer: model emits 1 stem (%s); derived '%s' as the residual",
                target.name.c_str(), residual.name.c_str());
            prepared.push_back(std::move(residual));
        }

        for (size_t index = 0; index < prepared.size(); ++index) {
            if (shouldCancel()) {
                result.canceled = true;
                result.stems.clear();
                return result;
            }

            const auto& entry = prepared[index];
            auto outPath = request.outputDirectory
                / std::format("{}_{}.wav", baseName, entry.name);
            if (!uapmd_stems::writeStereoWav(outPath, entry.left.data(), entry.right.data(),
                                             entry.left.size(), sampleRate)) {
                result.error = std::format("Failed to write stem {}", entry.name);
                result.stems.clear();
                return result;
            }

            result.stems.push_back(StemFile{entry.name, outPath});
            emitProgress(kProgressAfterSeparation
                             + (1.0f - kProgressAfterSeparation)
                                   * static_cast<float>(index + 1) / static_cast<float>(prepared.size()),
                         std::format("Writing stem {}...", entry.name));
        }

        result.success = !result.stems.empty();
        if (!result.success)
            result.error = "No stems were generated";
        return result;
    } catch (const std::exception& ex) {
        if (cancelRequested) {
            result.canceled = true;
            result.success = false;
            return result;
        }
        result.error = std::format("BS-Roformer failed: {}", ex.what());
        return result;
    }
}

} // namespace uapmd_bsroformer
