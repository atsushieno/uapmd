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

    auto stream = std::make_shared<std::ifstream>(outputPath, std::ios::binary);
    ASSERT_TRUE(*stream);
    auto reader = choc::audio::WAVAudioFileFormat<false>().createReader(stream);
    ASSERT_NE(reader, nullptr);
    const auto& properties = reader->getProperties();
    ASSERT_EQ(properties.numChannels, outputChannels);
    ASSERT_GT(properties.numFrames, 0u);

    std::vector<std::vector<float>> channels(
        properties.numChannels,
        std::vector<float>(properties.numFrames, 0.0f));
    std::vector<float*> channelPointers;
    channelPointers.reserve(channels.size());
    for (auto& channel : channels)
        channelPointers.push_back(channel.data());

    ASSERT_TRUE(reader->readFrames(
        0,
        choc::buffer::createChannelArrayView(channelPointers.data(), properties.numChannels, properties.numFrames)));

    float peak = 0.0f;
    for (const auto& channel : channels)
        for (const auto sample : channel)
            peak = std::max(peak, std::fabs(sample));

    EXPECT_GT(peak, 0.01f);
}

} // namespace
