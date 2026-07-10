#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <numbers>
#include <vector>

#include <gtest/gtest.h>
#include <choc/audio/choc_AudioFileFormat_WAV.h>

#include "uapmd-engine/uapmd-engine.hpp"

namespace fs = std::filesystem;

namespace {

struct RenderedAudio {
    choc::audio::AudioFileProperties properties{};
    std::vector<std::vector<float>> channels{};
};

class SineAudioFileReader final : public uapmd::AudioFileReader {
public:
    SineAudioFileReader(uint64_t numFrames, uint32_t numChannels, uint32_t sampleRate, double frequency, float amplitude)
        : properties_{numFrames, numChannels, sampleRate}
        , frequency_(frequency)
        , amplitude_(amplitude) {
    }

    Properties getProperties() const override {
        return properties_;
    }

    void readFrames(uint64_t startFrame,
                    uint64_t framesToRead,
                    float* const* dest,
                    uint32_t numChannels) override {
        const auto channels = std::min(numChannels, properties_.numChannels);
        for (uint64_t frame = 0; frame < framesToRead; ++frame) {
            const auto phase =
                2.0 * std::numbers::pi * frequency_ *
                static_cast<double>(startFrame + frame) / static_cast<double>(properties_.sampleRate);
            const auto sample = amplitude_ * static_cast<float>(std::sin(phase));
            for (uint32_t ch = 0; ch < channels; ++ch)
                dest[ch][frame] = sample;
        }
        for (uint32_t ch = channels; ch < numChannels; ++ch)
            std::fill_n(dest[ch], framesToRead, 0.0f);
    }

private:
    Properties properties_{};
    double frequency_{440.0};
    float amplitude_{0.25f};
};

RenderedAudio readRenderedAudioFile(const fs::path& outputPath) {
    auto stream = std::make_shared<std::ifstream>(outputPath, std::ios::binary);
    EXPECT_TRUE(*stream);
    if (!*stream)
        return {};
    auto reader = choc::audio::WAVAudioFileFormat<false>().createReader(stream);
    EXPECT_NE(reader, nullptr);
    if (!reader)
        return {};

    RenderedAudio rendered{};
    rendered.properties = reader->getProperties();
    rendered.channels.assign(
        rendered.properties.numChannels,
        std::vector<float>(rendered.properties.numFrames, 0.0f));

    std::vector<float*> channelPointers;
    channelPointers.reserve(rendered.channels.size());
    for (auto& channel : rendered.channels)
        channelPointers.push_back(channel.data());

    const bool readSuccess = reader->readFrames(
        0,
        choc::buffer::createChannelArrayView(
            channelPointers.data(),
            rendered.properties.numChannels,
            rendered.properties.numFrames));
    EXPECT_TRUE(readSuccess);
    return rendered;
}

float peakInFrameRange(const RenderedAudio& rendered, uint64_t startFrame, uint64_t endFrame) {
    float peak = 0.0f;
    const uint64_t clampedEnd = std::min<uint64_t>(endFrame, rendered.properties.numFrames);
    for (const auto& channel : rendered.channels)
        for (uint64_t frame = startFrame; frame < clampedEnd; ++frame)
            peak = std::max(peak, std::fabs(channel[frame]));
    return peak;
}

class SequencerEngineOutputTest : public ::testing::Test {
protected:
    fs::path test_dir_;

    void SetUp() override {
        test_dir_ = fs::temp_directory_path() / "uapmd_engine_output_test";
        fs::create_directories(test_dir_);
    }

    void TearDown() override {
        if (fs::exists(test_dir_))
            fs::remove_all(test_dir_);
    }
};

TEST_F(SequencerEngineOutputTest, OfflineRenderProducesAudibleSamples) {
    constexpr int32_t sampleRate = 48000;
    constexpr uint32_t bufferSize = 256;
    constexpr uint32_t outputChannels = 2;
    constexpr uint32_t umpBufferSize = 65536;
    constexpr uint64_t clipFrames = sampleRate / 10; // 100 ms

    auto engine = uapmd::SequencerEngine::create(sampleRate, bufferSize, umpBufferSize);
    ASSERT_NE(engine, nullptr);
    engine->setEngineActive(true);

    const auto trackIndex = engine->addEmptyTrack();
    ASSERT_GE(trackIndex, 0);

    auto addResult = engine->timeline().addAudioClipToTrack(
        trackIndex,
        uapmd::TimelinePosition::fromSamples(0, sampleRate),
        std::make_unique<SineAudioFileReader>(clipFrames, outputChannels, sampleRate, 440.0, 0.25f),
        "synthetic://sine");
    ASSERT_TRUE(addResult.success) << addResult.error;

    const auto outputPath = test_dir_ / "render.wav";
    uapmd::OfflineRenderSettings settings;
    settings.outputPath = outputPath;
    settings.startSeconds = 0.0;
    settings.endSeconds = 0.1;
    settings.sampleRate = sampleRate;
    settings.bufferSize = bufferSize;
    settings.outputChannels = outputChannels;
    settings.umpBufferSize = umpBufferSize;
    settings.infiniteTailPolicy = uapmd::OfflineInfiniteTailPolicy::LATENCY_FALLBACK;

    const auto result = uapmd::renderOfflineProject(*engine, settings);
    ASSERT_TRUE(result.success) << result.errorMessage;
    ASSERT_TRUE(fs::exists(outputPath));

    const auto rendered = readRenderedAudioFile(outputPath);
    ASSERT_EQ(rendered.properties.numChannels, outputChannels);
    ASSERT_GT(rendered.properties.numFrames, 0u);
    EXPECT_GT(peakInFrameRange(rendered, 0, rendered.properties.numFrames), 0.01f);
}

TEST_F(SequencerEngineOutputTest, OfflineRenderKeepsWarpedTailAudible) {
    constexpr int32_t sampleRate = 48000;
    constexpr uint32_t bufferSize = 256;
    constexpr uint32_t outputChannels = 2;
    constexpr uint32_t umpBufferSize = 65536;
    constexpr uint64_t clipFrames = sampleRate / 10; // 100 ms

    auto engine = uapmd::SequencerEngine::create(sampleRate, bufferSize, umpBufferSize);
    ASSERT_NE(engine, nullptr);
    engine->setEngineActive(true);

    const auto trackIndex = engine->addEmptyTrack();
    ASSERT_GE(trackIndex, 0);

    auto timelineTracks = engine->timeline().tracks();
    ASSERT_LT(static_cast<size_t>(trackIndex), timelineTracks.size());
    auto* timelineTrack = timelineTracks[static_cast<size_t>(trackIndex)];
    ASSERT_NE(timelineTrack, nullptr);

    std::vector<uapmd::AudioWarpPoint> warps{
        uapmd::AudioWarpPoint{0.0, 1.0, uapmd::AudioWarpReferenceType::ClipStart, {}, {}},
        uapmd::AudioWarpPoint{0.05, 0.5, uapmd::AudioWarpReferenceType::ClipStart, {}, {}},
    };

    constexpr int32_t sourceNodeId = 1001;
    auto sourceNode = std::make_unique<uapmd::AudioFileSourceNode>(
        sourceNodeId,
        std::make_unique<SineAudioFileReader>(clipFrames, outputChannels, sampleRate, 440.0, 0.25f),
        static_cast<double>(sampleRate),
        warps);

    uapmd::ClipData clip;
    clip.position = uapmd::TimelinePosition::fromSamples(0, sampleRate);
    clip.durationSamples = sourceNode->totalLength();
    clip.sourceNodeInstanceId = sourceNodeId;
    clip.filepath = "synthetic://warped-sine";
    clip.audioWarps = warps;
    clip.setTimeReference(uapmd::TimeReference::fromContainerStart({}, 0.0), sampleRate);

    const int32_t clipId = timelineTrack->addClip(clip, std::move(sourceNode));
    ASSERT_GE(clipId, 0);

    const auto outputPath = test_dir_ / "warped_render.wav";
    uapmd::OfflineRenderSettings settings;
    settings.outputPath = outputPath;
    settings.startSeconds = 0.0;
    settings.endSeconds = 0.15;
    settings.sampleRate = sampleRate;
    settings.bufferSize = bufferSize;
    settings.outputChannels = outputChannels;
    settings.umpBufferSize = umpBufferSize;
    settings.infiniteTailPolicy = uapmd::OfflineInfiniteTailPolicy::LATENCY_FALLBACK;

    const auto result = uapmd::renderOfflineProject(*engine, settings);
    ASSERT_TRUE(result.success) << result.errorMessage;
    ASSERT_TRUE(fs::exists(outputPath));

    const auto rendered = readRenderedAudioFile(outputPath);
    ASSERT_EQ(rendered.properties.numChannels, outputChannels);
    ASSERT_GT(rendered.properties.numFrames, static_cast<uint64_t>(sampleRate * 0.14));

    const uint64_t stretchedTailStart = static_cast<uint64_t>(sampleRate * 0.11);
    const uint64_t stretchedTailEnd = static_cast<uint64_t>(sampleRate * 0.145);
    EXPECT_GT(peakInFrameRange(rendered, stretchedTailStart, stretchedTailEnd), 0.01f);
}

} // namespace
