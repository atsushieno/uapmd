#include "../../../include/uapmd-data/priv/project/DemucsStemSeparator.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <format>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <system_error>

#include <choc/audio/choc_AudioFileFormat_WAV.h>
#include <choc/audio/choc_SampleBuffers.h>

#include "uapmd-data/priv/audio/AudioFileFactory.hpp"

#include <dsp.hpp>
#include <model.hpp>
#include <tensor.hpp>

namespace uapmd::import {

namespace {

constexpr uint32_t kTargetChannels = 2;
constexpr size_t kWriteBlockFrames = 8192;

std::pair<std::vector<float>, std::vector<float>> convertToStereo(
    const std::vector<std::vector<float>>& channels)
{
    if (channels.empty()) {
        return {{}, {}};
    }

    const size_t frameCount = channels.front().size();
    std::vector<float> left(frameCount, 0.0f);
    std::vector<float> right(frameCount, 0.0f);
    size_t leftContrib = 0;
    size_t rightContrib = 0;

    for (size_t ch = 0; ch < channels.size(); ++ch) {
        const auto& src = channels[ch];
        const size_t limit = std::min(src.size(), frameCount);
        if (ch % 2 == 0) {
            for (size_t i = 0; i < limit; ++i) {
                left[i] += src[i];
            }
            ++leftContrib;
        } else {
            for (size_t i = 0; i < limit; ++i) {
                right[i] += src[i];
            }
            ++rightContrib;
        }
    }

    if (leftContrib == 0) {
        leftContrib = 1;
    }
    if (rightContrib == 0) {
        rightContrib = 1;
    }

    const float leftScale = 1.0f / static_cast<float>(leftContrib);
    const float rightScale = 1.0f / static_cast<float>(rightContrib);

    for (size_t i = 0; i < frameCount; ++i) {
        left[i] *= leftScale;
        right[i] *= rightScale;
    }

    if (rightContrib == 0 && leftContrib > 0) {
        right = left;
    } else if (leftContrib == 0 && rightContrib > 0) {
        left = right;
    }

    return {left, right};
}

std::vector<float> resampleChannel(const std::vector<float>& input, uint32_t sourceRate)
{
    if (input.empty()) {
        return {};
    }

    if (sourceRate == demucscpp::SUPPORTED_SAMPLE_RATE) {
        return input;
    }

    const double ratio = static_cast<double>(demucscpp::SUPPORTED_SAMPLE_RATE) / static_cast<double>(sourceRate);
    size_t targetFrames = static_cast<size_t>(std::llround(input.size() * ratio));
    targetFrames = std::max<size_t>(1, targetFrames);

    std::vector<float> output(targetFrames, 0.0f);
    const double step = static_cast<double>(sourceRate) / static_cast<double>(demucscpp::SUPPORTED_SAMPLE_RATE);

    for (size_t i = 0; i < targetFrames; ++i) {
        const double sourcePos = i * step;
        const size_t index = static_cast<size_t>(sourcePos);
        const double frac = sourcePos - static_cast<double>(index);
        if (index >= input.size() - 1) {
            output[i] = input.back();
        } else {
            const float sample0 = input[index];
            const float sample1 = input[index + 1];
            output[i] = sample0 + static_cast<float>(frac) * (sample1 - sample0);
        }
    }
    return output;
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

bool writeStereoFile(const Eigen::MatrixXf& data, const std::filesystem::path& path)
{
    choc::audio::AudioFileProperties props;
    props.sampleRate = demucscpp::SUPPORTED_SAMPLE_RATE;
    props.numChannels = kTargetChannels;
    props.numFrames = static_cast<uint64_t>(data.cols());
    props.formatName = "wav";
    auto writer = choc::audio::WAVAudioFileFormat<true>().createWriter(path.string(), props);
    if (!writer) {
        return false;
    }

    choc::buffer::ChannelArrayBuffer<float> buffer(kTargetChannels, kWriteBlockFrames);
    uint64_t written = 0;
    const uint64_t totalFrames = static_cast<uint64_t>(data.cols());

    while (written < totalFrames) {
        const uint64_t framesRemaining = totalFrames - written;
        const uint32_t blockFrames = static_cast<uint32_t>(std::min<uint64_t>(framesRemaining, kWriteBlockFrames));
        for (uint32_t ch = 0; ch < kTargetChannels; ++ch) {
            for (uint32_t i = 0; i < blockFrames; ++i) {
                buffer.getSample(ch, i) = data(static_cast<int>(ch), static_cast<int>(written + i));
            }
        }
        auto view = buffer.getView().getStart(blockFrames);
        if (!writer->appendFrames(view)) {
            return false;
        }
        written += blockFrames;
    }

    return writer->flush();
}

std::vector<std::string> stemNames(bool isFourSource)
{
    if (isFourSource) {
        return {"drums", "bass", "other", "vocals"};
    }
    return {"drums", "bass", "other", "vocals", "guitar", "piano"};
}

} // namespace

DemucsStemSeparator::DemucsStemSeparator(std::string modelPath)
    : modelPath_(std::move(modelPath))
{
}

DemucsStemSeparator::Result DemucsStemSeparator::separate(
    const std::string& audioFile,
    const std::filesystem::path& outputDir) const
{
    Result result;
    if (modelPath_.empty()) {
        result.error = "Demucs model path is empty";
        return result;
    }

    auto reader = uapmd::createAudioFileReaderFromPath(audioFile);
    if (!reader) {
        result.error = "Unsupported audio format";
        return result;
    }

    const auto props = reader->getProperties();
    if (props.numChannels == 0 || props.numFrames == 0) {
        result.error = "Audio file has no data";
        return result;
    }

    std::vector<std::vector<float>> channelData(props.numChannels, std::vector<float>(props.numFrames, 0.0f));
    std::vector<float*> destPtrs;
    destPtrs.reserve(props.numChannels);
    for (auto& channel : channelData) {
        destPtrs.push_back(channel.data());
    }
    reader->readFrames(0, props.numFrames, destPtrs.data(), props.numChannels);

    auto [left, right] = convertToStereo(channelData);
    left = resampleChannel(left, props.sampleRate);
    right = resampleChannel(right, props.sampleRate);

    try {
        auto waveform = buildEigenWaveform(left, right);
        if (waveform.size() == 0) {
            result.error = "Failed to prepare input audio";
            return result;
        }

        demucscpp::demucs_model model{};
        if (!demucscpp::load_demucs_model(modelPath_, &model)) {
            result.error = "Unable to load Demucs model";
            return result;
        }

        demucscpp::ProgressCallback progress = [](float progressValue, const std::string& message) {
            std::cout << std::format("[Demucs {:>5.1f}%] {}\n", progressValue * 100.0f, message);
        };

        auto separation = demucscpp::demucs_inference(model, waveform, progress);
        const int nbSources = separation.dimension(0);
        const int nbChannels = separation.dimension(1);
        const int nbFrames = separation.dimension(2);

        if (nbChannels != static_cast<int>(kTargetChannels)) {
            result.error = "Demucs returned unexpected channel count";
            return result;
        }

        std::error_code ec;
        std::filesystem::create_directories(outputDir, ec);
        if (ec) {
            result.error = std::format("Failed to create output directory: {}", ec.message());
            return result;
        }

        const auto stemLabels = stemNames(model.is_4sources);
        const int stemsToWrite = std::min(static_cast<int>(stemLabels.size()), nbSources);

        for (int target = 0; target < stemsToWrite; ++target) {
            Eigen::MatrixXf stemData(kTargetChannels, nbFrames);
            for (int channel = 0; channel < nbChannels; ++channel) {
                for (int frame = 0; frame < nbFrames; ++frame) {
                    stemData(channel, frame) = separation(target, channel, frame);
                }
            }

            auto outPath = outputDir / std::format("{}_{}.wav",
                                                   std::filesystem::path(audioFile).stem().string(),
                                                   stemLabels[target]);
            if (!writeStereoFile(stemData, outPath)) {
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
    } catch (const std::exception& ex) {
        result.error = std::format("Demucs failed: {}", ex.what());
        return result;
    }
}

} // namespace uapmd::import
