#include "DemucsStemSeparator.hpp"

#include "StemSeparationSupport.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <format>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <system_error>

#include <dsp.hpp>
#include <model.hpp>
#include <tensor.hpp>

namespace uapmd_demucs {

using uapmd::import::StemFile;
using uapmd::import::StemSeparationCancelCallback;
using uapmd::import::StemSeparationRequest;
using uapmd::import::StemSeparationResult;
using uapmd::import::StemSeparatorModelFileSpec;

namespace {

constexpr uint32_t kTargetChannels = 2;

struct SeparationCanceled : std::exception {
    const char* what() const noexcept override {
        return "Demucs separation canceled";
    }
};

void checkShouldCancel(const StemSeparationCancelCallback& shouldCancel) {
    if (shouldCancel && shouldCancel()) {
        throw SeparationCanceled{};
    }
}

Eigen::MatrixXf buildEigenWaveform(const std::vector<float>& left,
                                   const std::vector<float>& right)
{
    const size_t frameCount = std::min(left.size(), right.size());
    if (frameCount == 0) {
        return {};
    }

    if (frameCount > static_cast<size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("Audio file is too long for Demucs processing");
    }

    Eigen::MatrixXf audio(kTargetChannels, static_cast<int>(frameCount));
    for (size_t i = 0; i < frameCount; ++i) {
        audio(0, static_cast<int>(i)) = left[i];
        audio(1, static_cast<int>(i)) = right[i];
    }
    return audio;
}

std::vector<std::string> stemNames(bool isFourSource)
{
    if (isFourSource) {
        return {"drums", "bass", "other", "vocals"};
    }
    return {"drums", "bass", "other", "vocals", "guitar", "piano"};
}

} // namespace

std::string_view DemucsStemSeparator::id() const noexcept {
    return "demucs";
}

std::string_view DemucsStemSeparator::name() const noexcept {
    return "Demucs";
}

StemSeparatorModelFileSpec DemucsStemSeparator::modelFileSpec() const {
    return {"Demucs ggml Model", {"*.bin"}};
}

StemSeparationResult DemucsStemSeparator::separate(const StemSeparationRequest& request) const
{
    const auto& audioFile = request.audioFile;
    const auto& outputDir = request.outputDirectory;
    const auto& progressCallback = request.progressCallback;
    const auto& shouldCancel = request.shouldCancel;

    StemSeparationResult result;
    if (request.modelPath.empty()) {
        result.error = "Demucs model path is empty";
        return result;
    }

    uapmd_stems::StereoAudio input;
    if (!uapmd_stems::loadStereoAudio(audioFile, demucscpp::SUPPORTED_SAMPLE_RATE, input, result.error))
        return result;

    auto emitProgress = [&](float progressValue, const std::string& message) {
        checkShouldCancel(shouldCancel);
        if (progressCallback) {
            if (!progressCallback(progressValue, message)) {
                throw SeparationCanceled{};
            }
        } else {
            std::cout << std::format("[Demucs {:>5.1f}%] {}\n", progressValue * 100.0f, message);
        }
    };

    try {
        checkShouldCancel(shouldCancel);

        auto waveform = buildEigenWaveform(input.left, input.right);
        if (waveform.size() == 0) {
            result.error = "Failed to prepare input audio";
            return result;
        }

        demucscpp::demucs_model model{};
        if (!demucscpp::load_demucs_model(request.modelPath, &model)) {
            result.error = "Unable to load Demucs model";
            return result;
        }

        demucscpp::ProgressCallback progress = [emitProgress](float progressValue, const std::string& message) {
            emitProgress(progressValue, message);
        };

        auto separation = demucscpp::demucs_inference(model, waveform, progress);
        const int nbSources = separation.dimension(0);
        const int nbChannels = separation.dimension(1);
        const int nbFrames = separation.dimension(2);

        if (nbChannels != static_cast<int>(kTargetChannels)) {
            result.error = "Demucs returned unexpected channel count";
            return result;
        }

        checkShouldCancel(shouldCancel);

        std::error_code ec;
        std::filesystem::create_directories(outputDir, ec);
        if (ec) {
            result.error = std::format("Failed to create output directory: {}", ec.message());
            return result;
        }

        const auto stemLabels = stemNames(model.is_4sources);
        const int stemsToWrite = std::min(static_cast<int>(stemLabels.size()), nbSources);

        for (int target = 0; target < stemsToWrite; ++target) {
            checkShouldCancel(shouldCancel);
            std::vector<float> stemLeft(static_cast<size_t>(nbFrames));
            std::vector<float> stemRight(static_cast<size_t>(nbFrames));
            for (int frame = 0; frame < nbFrames; ++frame) {
                stemLeft[static_cast<size_t>(frame)] = separation(target, 0, frame);
                stemRight[static_cast<size_t>(frame)] = separation(target, 1, frame);
            }

            auto outPath = outputDir / std::format("{}_{}.wav",
                                                   std::filesystem::path(audioFile).stem().string(),
                                                   stemLabels[target]);
            if (!uapmd_stems::writeStereoWav(outPath, stemLeft.data(), stemRight.data(),
                                             static_cast<size_t>(nbFrames),
                                             demucscpp::SUPPORTED_SAMPLE_RATE)) {
                result.error = std::format("Failed to write stem {}", stemLabels[target]);
                result.stems.clear();
                return result;
            }

            result.stems.push_back(StemFile{stemLabels[target], outPath});
        }

        result.success = !result.stems.empty();
        if (!result.success && result.error.empty()) {
            result.error = "No stems were generated";
        }
        return result;
    } catch (const SeparationCanceled&) {
        result.canceled = true;
        result.success = false;
        return result;
    } catch (const std::exception& ex) {
        result.error = std::format("Demucs failed: {}", ex.what());
        return result;
    }
}

} // namespace uapmd_demucs
