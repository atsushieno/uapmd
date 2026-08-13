#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <numbers>
#include <queue>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <choc/audio/choc_AudioFileFormat_WAV.h>

#include "uapmd-engine/uapmd-engine.hpp"
#include "uapmd-graph/uapmd-graph.hpp"

using namespace uapmd_graph;

namespace fs = std::filesystem;

namespace {

class TestEventLoop final : public remidy::EventLoop {
protected:
    void initializeOnUIThreadImpl() override {}

    bool runningOnMainThreadImpl() override {
        return std::this_thread::get_id() == main_thread_id_;
    }

    void enqueueTaskOnMainThreadImpl(std::function<void()>&& task) override {
        std::lock_guard lock(mutex_);
        tasks_.push(std::move(task));
    }

    void startImpl() override {}
    void stopImpl() override {}

    void processQueuedTasksImpl() override {
        std::queue<std::function<void()>> tasks;
        {
            std::lock_guard lock(mutex_);
            std::swap(tasks, tasks_);
        }
        while (!tasks.empty()) {
            auto task = std::move(tasks.front());
            tasks.pop();
            task();
        }
    }

private:
    std::thread::id main_thread_id_{std::this_thread::get_id()};
    std::mutex mutex_;
    std::queue<std::function<void()>> tasks_;
};

class ScopedTestEventLoop final {
public:
    ScopedTestEventLoop() {
        remidy::EventLoop::initializeOnUIThread();
        previous_ = remidy::getEventLoop();
        remidy::setEventLoop(&event_loop_);
    }

    ~ScopedTestEventLoop() {
        remidy::setEventLoop(previous_);
    }

private:
    remidy::EventLoop* previous_;
    TestEventLoop event_loop_;
};

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

class MutableTimingPlugin final : public uapmd_plugin_hosting::AudioPluginInstanceAPI {
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
    std::vector<uapmd_plugin_hosting::ParameterMetadata> parameterMetadataList() override { return {}; }
    std::vector<uapmd_plugin_hosting::ParameterMetadata> perNoteControllerMetadataList(
        remidy::PerNoteControllerContextTypes,
        uint32_t) override {
        return {};
    }
    std::vector<uapmd_plugin_hosting::PresetsMetadata> presetMetadataList() override { return {}; }
    void loadPreset(int32_t) override {}
    void loadPreset(int32_t, std::function<void(std::string, void*)>) override {}
    std::vector<uint8_t> saveStateSync() override { return {}; }
    void loadStateSync(std::vector<uint8_t>&) override {}
    void requestState(
        uapmd_plugin_hosting::StateContextType,
        bool,
        void* callbackContext,
        std::function<void(std::vector<uint8_t>, std::string, void*)> receiver) override {
        receiver({}, {}, callbackContext);
    }
    void loadState(
        std::vector<uint8_t>,
        uapmd_plugin_hosting::StateContextType,
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

TEST_F(
    SequencerEngineOutputTest,
    FreezePolicyCanChangeDuringPlaybackWithoutStartingRender) {
    constexpr int32_t sampleRate = 48000;
    constexpr uint32_t bufferSize = 256;
    constexpr uint32_t umpBufferSize = 65536;

    ScopedTestEventLoop eventLoop;
    auto engine = uapmd::SequencerEngine::create(
        sampleRate, bufferSize, umpBufferSize);
    ASSERT_NE(engine, nullptr);
    engine->setEngineActive(true);
    const auto trackIndex = engine->addEmptyTrack();
    ASSERT_GE(trackIndex, 0);

    engine->startPlayback();
    auto& manager = engine->frozenTrackManager();
    EXPECT_TRUE(manager.setFreezePolicyForTrack(
        trackIndex, uapmd::FrozenTrackManager::FreezePolicy::On));
    EXPECT_EQ(
        manager.freezePolicyForTrack(trackIndex),
        uapmd::FrozenTrackManager::FreezePolicy::On);
    EXPECT_EQ(
        manager.runtimeStateForTrack(trackIndex),
        uapmd::FrozenTrackManager::RuntimeState::Live);

    EXPECT_TRUE(manager.setFreezePolicyForTrack(
        trackIndex, uapmd::FrozenTrackManager::FreezePolicy::Off));
    EXPECT_EQ(
        manager.freezePolicyForTrack(trackIndex),
        uapmd::FrozenTrackManager::FreezePolicy::Off);
    EXPECT_EQ(
        manager.runtimeStateForTrack(trackIndex),
        uapmd::FrozenTrackManager::RuntimeState::Live);

    EXPECT_TRUE(manager.setFreezePolicyForTrack(
        trackIndex, uapmd::FrozenTrackManager::FreezePolicy::On));
    EXPECT_EQ(
        manager.runtimeStateForTrack(trackIndex),
        uapmd::FrozenTrackManager::RuntimeState::Live);
    engine->stopPlayback();

    remidy::AudioProcessContext process(
        engine->data().masterContext(), umpBufferSize);
    process.configureMainBus(2, 2, bufferSize);
    process.frameCount(bufferSize);
    constexpr auto kSilenceBlocks = sampleRate / 4 / bufferSize + 1;
    for (int block = 0; block < kSilenceBlocks; ++block)
        engine->processAudio(process);

    // The quiet notification crosses the tail-manager dispatch thread before
    // it is posted to the main event loop, where it begins the deferred render.
    for (int attempt = 0;
         attempt < 100 &&
         manager.runtimeStateForTrack(trackIndex) !=
             uapmd::FrozenTrackManager::RuntimeState::Rendering;
         ++attempt) {
        remidy::EventLoop::processQueuedTasks();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(
        manager.runtimeStateForTrack(trackIndex),
        uapmd::FrozenTrackManager::RuntimeState::Rendering);
}

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

    auto graph = AudioPluginFullDAGraph::create(4096);
    ASSERT_NE(graph, nullptr);
    auto* busesLayout = graph->getExtension<AudioBusesLayoutExtension>();
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

// ── Clip fragments ────────────────────────────────────────────────────────────

namespace {
    // Two MIDI 2.0 channel voice messages, two words each, a beat apart.
    const std::vector<uapmd_ump_t> kFragmentUmp{
        0x40903C00u, 0x7FFF0000u,
        0x40803C00u, 0x00000000u
    };
    const std::vector<uint64_t> kFragmentTicks{0, 0, 480, 480};

    uapmd::TimelineFacade::ClipAddResult addFragmentTestClip(
        uapmd::SequencerEngine& engine, int32_t trackIndex, int32_t sampleRate) {
        return engine.timeline().addMidiClipToTrack(
            trackIndex,
            uapmd::TimelinePosition::fromSamples(0, sampleRate),
            kFragmentUmp,
            kFragmentTicks,
            480,
            120.0,
            {},
            {},
            "Fragment Source");
    }
}

TEST_F(SequencerEngineOutputTest, ClipFragmentRestoresUnderItsOriginalIdentity) {
    constexpr int32_t sampleRate = 48000;
    auto engine = uapmd::SequencerEngine::create(sampleRate, 256, 65536);
    ASSERT_NE(engine, nullptr);
    const auto trackIndex = engine->addEmptyTrack();
    ASSERT_GE(trackIndex, 0);

    const auto added = addFragmentTestClip(*engine, trackIndex, sampleRate);
    ASSERT_TRUE(added.success) << added.error;
    auto& timeline = engine->timeline();
    ASSERT_TRUE(timeline.setClipName(trackIndex, added.clipId, "Renamed"));

    const auto fragment = timeline.captureClipFragment(trackIndex, added.clipId);
    ASSERT_TRUE(fragment.has_value());
    const auto capturedReferenceId = fragment->clip.referenceId;
    EXPECT_FALSE(capturedReferenceId.empty());
    EXPECT_EQ(fragment->clip.name, "Renamed");
    EXPECT_EQ(fragment->umpEvents, kFragmentUmp);

    // Capture must not disturb the document.
    ASSERT_NE(timeline.tracks()[trackIndex]->clipManager().getClip(added.clipId), nullptr);

    ASSERT_TRUE(timeline.removeClipFromTrack(trackIndex, added.clipId));
    ASSERT_EQ(timeline.tracks()[trackIndex]->clipManager().clipCount(), 0u);

    // Undoing a delete: the clip must come back as the same object.
    const auto restored = timeline.attachClipFragment(
        trackIndex, *fragment, uapmd::ProjectObjectIdPolicy::Restore);
    ASSERT_TRUE(restored.success) << restored.error;

    const auto* restoredClip =
        timeline.tracks()[trackIndex]->clipManager().getClip(restored.clipId);
    ASSERT_NE(restoredClip, nullptr);
    EXPECT_EQ(restoredClip->referenceId, capturedReferenceId);
    EXPECT_EQ(restoredClip->name, "Renamed");

    const auto recaptured = timeline.captureClipFragment(trackIndex, restored.clipId);
    ASSERT_TRUE(recaptured.has_value());
    EXPECT_EQ(recaptured->umpEvents, kFragmentUmp);
}

TEST_F(SequencerEngineOutputTest, ClipFragmentCarriesExtensionOwnedState) {
    // Stands in for a feature that owns state the document cannot see, such as
    // a plug-in's opaque per-clip state.
    struct RecordingExtension : uapmd::ProjectSerializationExtension {
        std::string capturedFrom;
        std::string restoredOnto;
        std::vector<uint8_t> restoredState;

        std::string_view extensionId() const override { return "test.fragment-state"; }

        bool captureClipFragmentState(const uapmd::ProjectObjectId& clipId,
                                      std::vector<uint8_t>& state,
                                      std::string&) override {
            capturedFrom = clipId;
            state = {0xDE, 0xAD, 0xBE, 0xEF};
            return true;
        }

        bool restoreClipFragmentState(const uapmd::ProjectObjectId& clipId,
                                      const std::vector<uint8_t>& state,
                                      std::string&) override {
            restoredOnto = clipId;
            restoredState = state;
            return true;
        }
    } extension;

    constexpr int32_t sampleRate = 48000;
    auto engine = uapmd::SequencerEngine::create(sampleRate, 256, 65536);
    ASSERT_NE(engine, nullptr);
    const auto trackIndex = engine->addEmptyTrack();
    ASSERT_GE(trackIndex, 0);
    auto& timeline = engine->timeline();
    timeline.addProjectSerializationExtension(extension);

    const auto added = addFragmentTestClip(*engine, trackIndex, sampleRate);
    ASSERT_TRUE(added.success) << added.error;

    const auto fragment = timeline.captureClipFragment(trackIndex, added.clipId);
    ASSERT_TRUE(fragment.has_value());
    const auto capturedReferenceId = fragment->clip.referenceId;
    EXPECT_EQ(extension.capturedFrom, capturedReferenceId);
    ASSERT_TRUE(fragment->extensionState.contains("test.fragment-state"));
    EXPECT_EQ(fragment->extensionState.at("test.fragment-state"),
              (std::vector<uint8_t>{0xDE, 0xAD, 0xBE, 0xEF}));

    // Pasting hands the state back addressed by the clip's *new* identity, so
    // that the extension attaches it to the object that now exists.
    const auto pasted = timeline.attachClipFragment(
        trackIndex, *fragment, uapmd::ProjectObjectIdPolicy::Mint);
    ASSERT_TRUE(pasted.success) << pasted.error;
    EXPECT_EQ(extension.restoredState, (std::vector<uint8_t>{0xDE, 0xAD, 0xBE, 0xEF}));
    EXPECT_FALSE(extension.restoredOnto.empty());
    EXPECT_NE(extension.restoredOnto, capturedReferenceId);

    // Restoring instead addresses it by the identity it was captured under.
    ASSERT_TRUE(timeline.removeClipFromTrack(trackIndex, added.clipId));
    const auto restored = timeline.attachClipFragment(
        trackIndex, *fragment, uapmd::ProjectObjectIdPolicy::Restore);
    ASSERT_TRUE(restored.success) << restored.error;
    EXPECT_EQ(extension.restoredOnto, capturedReferenceId);

    timeline.removeProjectSerializationExtension(extension);
}

TEST_F(SequencerEngineOutputTest, ClipFragmentCaptureFailsRatherThanLosingExtensionState) {
    // An extension that cannot capture its state must not yield a fragment that
    // merely looks complete: restoring one would silently discard the user's
    // work with no way to notice or recover it.
    struct FailingExtension : uapmd::ProjectSerializationExtension {
        std::string_view extensionId() const override { return "test.failing"; }
        bool captureClipFragmentState(const uapmd::ProjectObjectId&,
                                      std::vector<uint8_t>&,
                                      std::string& error) override {
            error = "cannot archive right now";
            return false;
        }
    } extension;

    constexpr int32_t sampleRate = 48000;
    auto engine = uapmd::SequencerEngine::create(sampleRate, 256, 65536);
    ASSERT_NE(engine, nullptr);
    const auto trackIndex = engine->addEmptyTrack();
    ASSERT_GE(trackIndex, 0);
    auto& timeline = engine->timeline();

    const auto added = addFragmentTestClip(*engine, trackIndex, sampleRate);
    ASSERT_TRUE(added.success) << added.error;

    // Without the extension the capture succeeds.
    ASSERT_TRUE(timeline.captureClipFragment(trackIndex, added.clipId).has_value());

    timeline.addProjectSerializationExtension(extension);
    EXPECT_FALSE(timeline.captureClipFragment(trackIndex, added.clipId).has_value());
    timeline.removeProjectSerializationExtension(extension);
}

TEST_F(SequencerEngineOutputTest, ClipFragmentCaptureIsRefusedInsideATransaction) {
    // Archiving a plug-in's state is illegal while its document is being
    // edited, which is exactly what an open transaction holds. The refusal
    // belongs at this boundary so the mistake is reported at the call site.
    constexpr int32_t sampleRate = 48000;
    auto engine = uapmd::SequencerEngine::create(sampleRate, 256, 65536);
    ASSERT_NE(engine, nullptr);
    const auto trackIndex = engine->addEmptyTrack();
    ASSERT_GE(trackIndex, 0);
    auto& timeline = engine->timeline();

    const auto added = addFragmentTestClip(*engine, trackIndex, sampleRate);
    ASSERT_TRUE(added.success) << added.error;
    ASSERT_TRUE(timeline.captureClipFragment(trackIndex, added.clipId).has_value());

    {
        uapmd::ScopedDocumentTransaction transaction(timeline);
        EXPECT_FALSE(timeline.captureClipFragment(trackIndex, added.clipId).has_value());
    }

    // And succeeds again once the transaction closes.
    EXPECT_TRUE(timeline.captureClipFragment(trackIndex, added.clipId).has_value());
}

TEST_F(SequencerEngineOutputTest, ClipFragmentMintsANewIdentityWhenAttachedAlongside) {
    constexpr int32_t sampleRate = 48000;
    auto engine = uapmd::SequencerEngine::create(sampleRate, 256, 65536);
    ASSERT_NE(engine, nullptr);
    const auto trackIndex = engine->addEmptyTrack();
    ASSERT_GE(trackIndex, 0);

    const auto added = addFragmentTestClip(*engine, trackIndex, sampleRate);
    ASSERT_TRUE(added.success) << added.error;
    auto& timeline = engine->timeline();

    const auto fragment = timeline.captureClipFragment(trackIndex, added.clipId);
    ASSERT_TRUE(fragment.has_value());

    // Pasting alongside the original: a second, distinct clip.
    const auto pasted = timeline.attachClipFragment(
        trackIndex, *fragment, uapmd::ProjectObjectIdPolicy::Mint);
    ASSERT_TRUE(pasted.success) << pasted.error;
    EXPECT_NE(pasted.clipId, added.clipId);
    EXPECT_EQ(timeline.tracks()[trackIndex]->clipManager().clipCount(), 2u);

    const auto* pastedClip =
        timeline.tracks()[trackIndex]->clipManager().getClip(pasted.clipId);
    ASSERT_NE(pastedClip, nullptr);
    EXPECT_FALSE(pastedClip->referenceId.empty());
    EXPECT_NE(pastedClip->referenceId, fragment->clip.referenceId);
    // Content still copies across even though identity does not.
    const auto pastedFragment = timeline.captureClipFragment(trackIndex, pasted.clipId);
    ASSERT_TRUE(pastedFragment.has_value());
    EXPECT_EQ(pastedFragment->umpEvents, kFragmentUmp);
}

// ── Track fragments ───────────────────────────────────────────────────────────

TEST_F(SequencerEngineOutputTest, TrackFragmentRoundTripsContentAndIdentity) {
    constexpr int32_t sampleRate = 48000;
    auto engine = uapmd::SequencerEngine::create(sampleRate, 256, 65536);
    ASSERT_NE(engine, nullptr);
    const auto trackIndex = engine->addEmptyTrack();
    ASSERT_GE(trackIndex, 0);
    auto& timeline = engine->timeline();

    ASSERT_TRUE(addFragmentTestClip(*engine, trackIndex, sampleRate).success);
    auto& tracks = engine->tracks();
    ASSERT_LT(static_cast<size_t>(trackIndex), tracks.size());
    tracks[trackIndex]->trackGain(0.25);
    tracks[trackIndex]->muted(true);

    // No plugins on this track, so the chain completes synchronously; the
    // callback shape is still the contract.
    std::optional<uapmd::ProjectTrackFragment> captured;
    std::string captureError;
    timeline.captureTrackFragment(trackIndex, [&](auto fragment, std::string error) {
        captured = std::move(fragment);
        captureError = std::move(error);
    });
    ASSERT_TRUE(captured.has_value()) << captureError;
    const auto capturedReferenceId = captured->referenceId;
    EXPECT_DOUBLE_EQ(captured->volume, 0.25);
    EXPECT_TRUE(captured->muted);
    ASSERT_EQ(captured->clips.size(), 1u);
    EXPECT_EQ(captured->clips[0].umpEvents, kFragmentUmp);

    // Mint: a second, distinct track carrying the same content.
    int32_t clonedIndex = -1;
    std::string attachError;
    timeline.attachTrackFragment(*captured, {}, [&](int32_t index, std::string error) {
        clonedIndex = index;
        attachError = std::move(error);
    });
    ASSERT_GE(clonedIndex, 0) << attachError;
    EXPECT_NE(clonedIndex, trackIndex);

    auto clonedTracks = timeline.tracks();
    ASSERT_LT(static_cast<size_t>(clonedIndex), clonedTracks.size());
    EXPECT_NE(clonedTracks[clonedIndex]->referenceId(), capturedReferenceId);
    EXPECT_EQ(clonedTracks[clonedIndex]->clipManager().clipCount(), 1u);
    EXPECT_DOUBLE_EQ(engine->tracks()[clonedIndex]->trackGain(), 0.25);
    EXPECT_TRUE(engine->tracks()[clonedIndex]->muted());
}

TEST_F(SequencerEngineOutputTest, TrackFragmentAttachHonoursTheComponentMask) {
    constexpr int32_t sampleRate = 48000;
    auto engine = uapmd::SequencerEngine::create(sampleRate, 256, 65536);
    ASSERT_NE(engine, nullptr);
    const auto trackIndex = engine->addEmptyTrack();
    ASSERT_GE(trackIndex, 0);
    auto& timeline = engine->timeline();
    ASSERT_TRUE(addFragmentTestClip(*engine, trackIndex, sampleRate).success);

    std::optional<uapmd::ProjectTrackFragment> captured;
    timeline.captureTrackFragment(trackIndex, [&](auto fragment, std::string) {
        captured = std::move(fragment);
    });
    ASSERT_TRUE(captured.has_value());
    ASSERT_EQ(captured->clips.size(), 1u);

    // "Duplicate without contents": the same track, set up the same way, empty.
    uapmd::ProjectTrackAttachOptions options;
    options.includeClips = false;
    int32_t emptyIndex = -1;
    timeline.attachTrackFragment(*captured, options, [&](int32_t index, std::string) {
        emptyIndex = index;
    });
    ASSERT_GE(emptyIndex, 0);

    auto tracks = timeline.tracks();
    ASSERT_LT(static_cast<size_t>(emptyIndex), tracks.size());
    EXPECT_EQ(tracks[emptyIndex]->clipManager().clipCount(), 0u);
    // The original is untouched by either capture or attach.
    EXPECT_EQ(tracks[trackIndex]->clipManager().clipCount(), 1u);
}

TEST_F(SequencerEngineOutputTest, TrackFragmentCaptureIsRefusedInsideATransaction) {
    constexpr int32_t sampleRate = 48000;
    auto engine = uapmd::SequencerEngine::create(sampleRate, 256, 65536);
    ASSERT_NE(engine, nullptr);
    const auto trackIndex = engine->addEmptyTrack();
    ASSERT_GE(trackIndex, 0);
    auto& timeline = engine->timeline();

    {
        uapmd::ScopedDocumentTransaction transaction(timeline);
        bool called = false;
        std::string error;
        timeline.captureTrackFragment(trackIndex, [&](auto fragment, std::string err) {
            called = true;
            EXPECT_FALSE(fragment.has_value());
            error = std::move(err);
        });
        EXPECT_TRUE(called);
        EXPECT_FALSE(error.empty());
    }

    bool succeeded = false;
    timeline.captureTrackFragment(trackIndex, [&](auto fragment, std::string) {
        succeeded = fragment.has_value();
    });
    EXPECT_TRUE(succeeded);
}

// ── Frozen render revocation ──────────────────────────────────────────────────

TEST_F(SequencerEngineOutputTest, ClipMutationsRevokeTheTracksFrozenRender) {
    // A frozen render records what a track produced at one moment, so any clip
    // change makes it wrong. Revocation is driven by document events rather
    // than by each call site remembering to ask for it.
    constexpr int32_t sampleRate = 48000;
    auto engine = uapmd::SequencerEngine::create(sampleRate, 256, 65536);
    ASSERT_NE(engine, nullptr);
    const auto trackIndex = engine->addEmptyTrack();
    ASSERT_GE(trackIndex, 0);
    auto& timeline = engine->timeline();
    auto& frozen = engine->frozenTrackManager();

    const auto generationBeforeAdd = frozen.invalidationGenerationForTrack(trackIndex);

    const auto added = addFragmentTestClip(*engine, trackIndex, sampleRate);
    ASSERT_TRUE(added.success) << added.error;
    const auto generationAfterAdd = frozen.invalidationGenerationForTrack(trackIndex);
    EXPECT_GT(generationAfterAdd, generationBeforeAdd);

    ASSERT_TRUE(timeline.setClipName(trackIndex, added.clipId, "Renamed"));
    const auto generationAfterChange = frozen.invalidationGenerationForTrack(trackIndex);
    EXPECT_GT(generationAfterChange, generationAfterAdd);

    ASSERT_TRUE(timeline.removeClipFromTrack(trackIndex, added.clipId));
    EXPECT_GT(frozen.invalidationGenerationForTrack(trackIndex), generationAfterChange);
}

} // namespace
