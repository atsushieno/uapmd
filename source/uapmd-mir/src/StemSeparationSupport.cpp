#include "StemSeparationSupport.hpp"

#include <cmath>
#include <format>

#include <choc/audio/choc_AudioFileFormat_WAV.h>
#include <choc/audio/choc_SampleBuffers.h>

#include <uapmd-data/uapmd-data.hpp>

namespace uapmd_stems {

namespace {

constexpr uint32_t kStereoChannels = 2;
constexpr size_t kWriteBlockFrames = 8192;

std::pair<std::vector<float>, std::vector<float>> downmixToStereo(
    const std::vector<std::vector<float>>& channels)
{
    if (channels.empty())
        return {{}, {}};

    const size_t frameCount = channels.front().size();
    std::vector<float> left(frameCount, 0.0f);
    std::vector<float> right(frameCount, 0.0f);
    size_t leftContrib = 0;
    size_t rightContrib = 0;

    for (size_t ch = 0; ch < channels.size(); ++ch) {
        const auto& src = channels[ch];
        const size_t limit = std::min(src.size(), frameCount);
        if (ch % 2 == 0) {
            for (size_t i = 0; i < limit; ++i)
                left[i] += src[i];
            ++leftContrib;
        } else {
            for (size_t i = 0; i < limit; ++i)
                right[i] += src[i];
            ++rightContrib;
        }
    }

    // A mono source contributes to the left only; mirror it rather than
    // handing the model a silent right channel.
    if (rightContrib == 0 && leftContrib > 0) {
        for (size_t i = 0; i < frameCount; ++i)
            left[i] /= static_cast<float>(leftContrib);
        right = left;
        return {left, right};
    }
    if (leftContrib == 0 && rightContrib > 0) {
        for (size_t i = 0; i < frameCount; ++i)
            right[i] /= static_cast<float>(rightContrib);
        left = right;
        return {left, right};
    }

    const float leftScale = 1.0f / static_cast<float>(std::max<size_t>(leftContrib, 1));
    const float rightScale = 1.0f / static_cast<float>(std::max<size_t>(rightContrib, 1));
    for (size_t i = 0; i < frameCount; ++i) {
        left[i] *= leftScale;
        right[i] *= rightScale;
    }
    return {left, right};
}

std::vector<float> resampleChannel(const std::vector<float>& input,
                                   uint32_t sourceRate,
                                   uint32_t targetRate)
{
    if (input.empty() || sourceRate == targetRate)
        return input;

    const double ratio = static_cast<double>(targetRate) / static_cast<double>(sourceRate);
    size_t targetFrames = static_cast<size_t>(std::llround(input.size() * ratio));
    targetFrames = std::max<size_t>(1, targetFrames);

    std::vector<float> output(targetFrames, 0.0f);
    const double step = static_cast<double>(sourceRate) / static_cast<double>(targetRate);

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

} // namespace

bool loadStereoAudio(const std::string& filepath,
                     uint32_t targetSampleRate,
                     StereoAudio& audio,
                     std::string& error)
{
    auto reader = uapmd::createAudioFileReaderFromPath(filepath);
    if (!reader) {
        error = "Unsupported audio format";
        return false;
    }

    const auto props = reader->getProperties();
    if (props.numChannels == 0 || props.numFrames == 0) {
        error = "Audio file has no data";
        return false;
    }

    std::vector<std::vector<float>> channelData(props.numChannels, std::vector<float>(props.numFrames, 0.0f));
    std::vector<float*> destPtrs;
    destPtrs.reserve(props.numChannels);
    for (auto& channel : channelData)
        destPtrs.push_back(channel.data());
    reader->readFrames(0, props.numFrames, destPtrs.data(), props.numChannels);

    auto [left, right] = downmixToStereo(channelData);
    audio.left = resampleChannel(left, props.sampleRate, targetSampleRate);
    audio.right = resampleChannel(right, props.sampleRate, targetSampleRate);
    if (audio.frameCount() == 0) {
        error = "Failed to prepare input audio";
        return false;
    }
    return true;
}

bool writeStereoWav(const std::filesystem::path& path,
                    const float* left,
                    const float* right,
                    size_t frameCount,
                    uint32_t sampleRate)
{
    choc::audio::AudioFileProperties props;
    props.sampleRate = sampleRate;
    props.numChannels = kStereoChannels;
    props.numFrames = static_cast<uint64_t>(frameCount);
    props.formatName = "wav";
    auto writer = choc::audio::WAVAudioFileFormat<true>().createWriter(path.string(), props);
    if (!writer)
        return false;

    const float* sources[kStereoChannels]{left, right};
    choc::buffer::ChannelArrayBuffer<float> buffer(kStereoChannels, kWriteBlockFrames);
    uint64_t written = 0;
    const uint64_t totalFrames = static_cast<uint64_t>(frameCount);

    while (written < totalFrames) {
        const uint64_t framesRemaining = totalFrames - written;
        const uint32_t blockFrames = static_cast<uint32_t>(std::min<uint64_t>(framesRemaining, kWriteBlockFrames));
        for (uint32_t ch = 0; ch < kStereoChannels; ++ch)
            for (uint32_t i = 0; i < blockFrames; ++i)
                buffer.getSample(ch, i) = sources[ch][written + i];
        auto view = buffer.getView().getStart(blockFrames);
        if (!writer->appendFrames(view))
            return false;
        written += blockFrames;
    }

    return writer->flush();
}

} // namespace uapmd_stems
