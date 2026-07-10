#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <numbers>
#include <vector>

#include <gtest/gtest.h>
#include <choc/audio/choc_AudioFileFormat_WAV.h>

#include "uapmd-engine/uapmd-engine.hpp"
#include "uapmd-graph/uapmd-graph.hpp"

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

class TestAudioBuses final : public remidy::PluginAudioBuses {
public:
    TestAudioBuses()
        : input_definition_("Input", remidy::AudioBusRole::Main, {remidy::AudioChannelLayout::stereo()})
        , output_definition_("Output", remidy::AudioBusRole::Main, {remidy::AudioChannelLayout::stereo()})
        , input_configuration_(input_definition_)
        , output_configuration_(output_definition_) {
        input_buses_.push_back(&input_configuration_);
        output_buses_.push_back(&output_configuration_);
    }

    bool hasEventInputs() override { return false; }
    bool hasEventOutputs() override { return false; }
    const std::vector<remidy::AudioBusConfiguration*>& audioInputBuses() const override {
        return input_buses_;
    }
    const std::vector<remidy::AudioBusConfiguration*>& audioOutputBuses() const override {
        return output_buses_;
    }

private:
    remidy::AudioBusDefinition input_definition_;
    remidy::AudioBusDefinition output_definition_;
    remidy::AudioBusConfiguration input_configuration_;
    remidy::AudioBusConfiguration output_configuration_;
    std::vector<remidy::AudioBusConfiguration*> input_buses_;
    std::vector<remidy::AudioBusConfiguration*> output_buses_;
};

class MutableTimingPlugin final : public uapmd::AudioPluginInstanceAPI {
public:
    uint32_t latencyInSamples() const override {
        return latency_in_samples_.load(std::memory_order_acquire);
    }

    void latencyInSamples(uint32_t value) {
        latency_in_samples_.store(value, std::memory_order_release);
    }

    std::string& displayName() const override { return display_name_; }
    std::string& formatName() const override { return format_name_; }
    std::string& pluginId() const override { return plugin_id_; }
    bool bypassed() const override { return bypassed_; }
    void bypassed(bool value) override { bypassed_ = value; }
    uapmd_status_t startProcessing() override { return 0; }
    uapmd_status_t stopProcessing() override { return 0; }
    uapmd_status_t processAudio(remidy::AudioProcessContext&) override { return 0; }
    double tailLengthInSeconds() const override { return 0.0; }
    bool requiresReplacingProcess() const override { return false; }
    std::vector<uapmd::ParameterMetadata> parameterMetadataList() override { return {}; }
    std::vector<uapmd::ParameterMetadata> perNoteControllerMetadataList(
        remidy::PerNoteControllerContextTypes,
        uint32_t) override {
        return {};
    }
    std::vector<uapmd::PresetsMetadata> presetMetadataList() override { return {}; }
    void loadPreset(int32_t) override {}
    void loadPreset(int32_t, std::function<void(std::string, void*)>) override {}
    std::vector<uint8_t> saveStateSync() override { return {}; }
    void loadStateSync(std::vector<uint8_t>&) override {}
    void requestState(
        uapmd::StateContextType,
        bool,
        void* callbackContext,
        std::function<void(std::vector<uint8_t>, std::string, void*)> receiver) override {
        receiver({}, {}, callbackContext);
    }
    void loadState(
        std::vector<uint8_t>,
        uapmd::StateContextType,
        bool,
        void* callbackContext,
        std::function<void(std::string, void*)> completed) override {
        completed({}, callbackContext);
    }
    double getParameterValue(int32_t) override { return 0.0; }
    void setParameterValue(int32_t, double) override {}
    void enqueueParameterValueRT(int32_t, double, uapmd_timestamp_t) override {}
    std::string getParameterValueString(int32_t, double) override { return {}; }
    void setPerNoteControllerValue(uint8_t, uint8_t, double) override {}
    void enqueuePerNoteControllerValueRT(uint8_t, uint8_t, double, uapmd_timestamp_t) override {}
    std::string getPerNoteControllerValueString(uint8_t, uint8_t, double) override { return {}; }
    bool hasUISupport() override { return false; }
    bool createUI(bool, void*, std::function<bool(uint32_t, uint32_t)>) override { return false; }
    void destroyUI() override {}
    bool showUI() override { return false; }
    void hideUI() override {}
    bool isUIVisible() const override { return false; }
    bool setUISize(uint32_t, uint32_t) override { return false; }
    bool getUISize(uint32_t&, uint32_t&) override { return false; }
    bool canUIResize() override { return false; }
    remidy::PluginParameterSupport* parameterSupport() override { return nullptr; }
    remidy::PluginAudioBuses* audioBuses() override { return &audio_buses_; }
    bool dirty() const override { return false; }
    void clearDirty() override {}
    remidy::EventListenerId addDirtyStateListener(std::function<void(bool)>) override { return 0; }
    void removeDirtyStateListener(remidy::EventListenerId) override {}
    remidy::EventListenerId addTimingInfoChangeListener(
        std::function<void(remidy::PluginTimingInfoChange)>) override {
        return 0;
    }
    void removeTimingInfoChangeListener(remidy::EventListenerId) override {}

private:
    mutable std::string display_name_{"Test Plugin"};
    mutable std::string format_name_{"Test"};
    mutable std::string plugin_id_{"test.plugin"};
    bool bypassed_{false};
    std::atomic<uint32_t> latency_in_samples_{0};
    TestAudioBuses audio_buses_{};
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

TEST_F(SequencerEngineOutputTest, FullDAGraphRefreshesRuntimeTimingInfo) {
    MutableTimingPlugin plugin;
    plugin.latencyInSamples(64);

    auto graph = uapmd::AudioPluginFullDAGraph::create(4096);
    ASSERT_NE(graph, nullptr);
    auto* busesLayout = graph->getExtension<uapmd::AudioBusesLayoutExtension>();
    ASSERT_NE(busesLayout, nullptr);
    busesLayout->applyBusesLayout({1, 1, 1, 1});

    ASSERT_EQ(graph->appendNodeSimple(1, &plugin, [] {}), 0);
    EXPECT_EQ(graph->mainOutputLatencyInSamples(), 64u);

    plugin.latencyInSamples(256);
    graph->refreshTimingInfo();
    EXPECT_EQ(graph->mainOutputLatencyInSamples(), 256u);
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
