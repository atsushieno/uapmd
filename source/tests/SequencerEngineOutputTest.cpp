#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
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

class TestPluginParameterSupport final : public remidy::PluginParameterSupport {
public:
    std::vector<remidy::PluginParameter*>& parameters() override { return parameters_; }

    std::vector<remidy::PluginParameter*>& perNoteControllers(
        remidy::PerNoteControllerContextTypes,
        remidy::PerNoteControllerContext) override {
        return parameters_;
    }

    remidy::StatusCode setParameter(uint32_t index, double value) override {
        parameter_values_[static_cast<int32_t>(index)] = value;
        return remidy::StatusCode::OK;
    }

    remidy::StatusCode enqueueParameterRT(uint32_t index, double value, uint64_t) override {
        return setParameter(index, value);
    }

    remidy::StatusCode getParameter(uint32_t index, double* value) override {
        if (!value)
            return remidy::StatusCode::INVALID_PARAMETER_OPERATION;
        *value = parameter_values_[static_cast<int32_t>(index)];
        return remidy::StatusCode::OK;
    }

    remidy::StatusCode setPerNoteController(
        remidy::PerNoteControllerContext context,
        uint32_t index,
        double value) override {
        per_note_values_[{static_cast<uint8_t>(context.note), static_cast<uint8_t>(index)}] = value;
        return remidy::StatusCode::OK;
    }

    remidy::StatusCode enqueuePerNoteControllerRT(
        remidy::PerNoteControllerContext context,
        uint32_t index,
        double value,
        uint64_t) override {
        return setPerNoteController(context, index, value);
    }

    remidy::StatusCode getPerNoteController(
        remidy::PerNoteControllerContext context,
        uint32_t index,
        double* value) override {
        if (!value)
            return remidy::StatusCode::INVALID_PARAMETER_OPERATION;
        *value = per_note_values_[{
            static_cast<uint8_t>(context.note), static_cast<uint8_t>(index)}];
        return remidy::StatusCode::OK;
    }

    std::string valueToString(uint32_t, double value) override {
        return std::to_string(value);
    }

    std::string valueToStringPerNote(
        remidy::PerNoteControllerContext,
        uint32_t,
        double value) override {
        return std::to_string(value);
    }

private:
    std::vector<remidy::PluginParameter*> parameters_{};
    std::map<int32_t, double> parameter_values_{};
    std::map<std::pair<uint8_t, uint8_t>, double> per_note_values_{};
};

class MutableTimingPlugin final : public uapmd_plugin_hosting::AudioPluginInstanceAPI {
public:
    explicit MutableTimingPlugin(bool deferStateLoad = false)
        : defer_state_load_(deferStateLoad) {
    }

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
    std::vector<uint8_t> saveStateSync() override { return state_; }
    void loadStateSync(std::vector<uint8_t>& state) override { state_ = state; }
    void requestState(
        uapmd_plugin_hosting::StateContextType,
        bool,
        void* callbackContext,
        std::function<void(std::vector<uint8_t>, std::string, void*)> receiver) override {
        receiver(state_, {}, callbackContext);
    }
    void loadState(
        std::vector<uint8_t> state,
        uapmd_plugin_hosting::StateContextType,
        bool,
        void* callbackContext,
        std::function<void(std::string, void*)> completed) override {
        if (defer_state_load_) {
            pending_state_ = std::move(state);
            pending_state_context_ = callbackContext;
            pending_state_callback_ = std::move(completed);
            return;
        }
        state_ = std::move(state);
        completed({}, callbackContext);
    }

    bool stateLoadPending() const { return static_cast<bool>(pending_state_callback_); }

    void completeStateLoad(std::string error = {}) {
        if (!pending_state_callback_)
            return;
        auto completed = std::move(pending_state_callback_);
        auto* context = pending_state_context_;
        pending_state_context_ = nullptr;
        if (error.empty())
            state_ = std::move(pending_state_);
        else
            pending_state_.clear();
        completed(std::move(error), context);
    }
    double getParameterValue(int32_t index) override {
        double value = 0.0;
        parameter_support_.getParameter(static_cast<uint32_t>(index), &value);
        return value;
    }
    void setParameterValue(int32_t index, double value) override {
        parameter_support_.setParameter(static_cast<uint32_t>(index), value);
    }
    void enqueueParameterValueRT(int32_t, double, uapmd_timestamp_t) override {}
    std::string getParameterValueString(int32_t, double) override { return {}; }
    void setPerNoteControllerValue(uint8_t note, uint8_t index, double value) override {
        parameter_support_.setPerNoteController(
            {.note = note, .channel = 0, .group = 0, .extra = 0}, index, value);
    }
    bool getPerNoteControllerValue(uint8_t note, uint8_t index, double* value) override {
        return parameter_support_.getPerNoteController(
                   {.note = note, .channel = 0, .group = 0, .extra = 0}, index, value)
            == remidy::StatusCode::OK;
    }
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
    remidy::PluginParameterSupport* parameterSupport() override { return &parameter_support_; }
    remidy::PluginAudioBuses* audioBuses() override { return &audio_buses_; }
    remidy::EventListenerId addTimingInfoChangeListener(
        std::function<void(remidy::PluginTimingInfoChange)>) override {
        return 0;
    }
    void removeTimingInfoChangeListener(remidy::EventListenerId) override {}

    void externallySetParameter(int32_t index, double value) {
        parameter_support_.setParameter(static_cast<uint32_t>(index), value);
        parameter_support_.parameterChangeEvent().notify(
            static_cast<uint32_t>(index), value);
    }

private:
    mutable std::string display_name_{"Test Plugin"};
    mutable std::string format_name_{"Test"};
    mutable std::string plugin_id_{"test.plugin"};
    bool bypassed_{false};
    std::atomic<uint32_t> latency_in_samples_{0};
    std::vector<uint8_t> state_{};
    TestPluginParameterSupport parameter_support_{};
    TestAudioBuses audio_buses_{};
    bool defer_state_load_{false};
    std::vector<uint8_t> pending_state_{};
    void* pending_state_context_{nullptr};
    std::function<void(std::string, void*)> pending_state_callback_{};
};

class TestPluginHostingAPI final : public uapmd_plugin_hosting::AudioPluginHostingAPI {
public:
    explicit TestPluginHostingAPI(
        bool deferCreation = false,
        bool deferStateLoad = false)
        : defer_creation_(deferCreation),
          defer_state_load_(deferStateLoad) {
    }

    std::vector<remidy::PluginCatalogEntry> pluginCatalogEntries() override { return {}; }
    void savePluginCatalogToFile(std::filesystem::path) override {}
    void performPluginScanning(bool) override {}
    void reloadPluginCatalogFromCache() override {}

    void createPluginInstance(
        uint32_t,
        uint32_t,
        std::optional<uint32_t>,
        std::optional<uint32_t>,
        bool,
        std::string&,
        std::string&,
        std::function<void(int32_t, std::string)>&& callback) override {
        const auto instanceId = next_instance_id_++;
        instances_[instanceId] =
            std::make_unique<MutableTimingPlugin>(defer_state_load_);
        if (defer_creation_) {
            pending_creations_.push_back({instanceId, std::move(callback)});
            return;
        }
        callback(instanceId, {});
    }

    void deletePluginInstance(int32_t instanceId) override {
        instances_.erase(instanceId);
    }

    uapmd_plugin_hosting::AudioPluginInstanceAPI* getInstance(int32_t instanceId) override {
        const auto it = instances_.find(instanceId);
        return it == instances_.end() ? nullptr : it->second.get();
    }

    remidy::EventListenerId addPluginStateChangeListener(std::function<void(int32_t)>) override {
        return 0;
    }

    void removePluginStateChangeListener(remidy::EventListenerId) override {}

    std::vector<int32_t> instanceIds() override {
        std::vector<int32_t> result;
        result.reserve(instances_.size());
        for (const auto& [instanceId, instance] : instances_)
            if (instance)
                result.push_back(instanceId);
        return result;
    }

    bool creationPending() const { return !pending_creations_.empty(); }

    void completeNextCreation(std::string error = {}) {
        if (pending_creations_.empty())
            return;
        auto pending = std::move(pending_creations_.front());
        pending_creations_.erase(pending_creations_.begin());
        if (!error.empty()) {
            instances_.erase(pending.instanceId);
            pending.callback(-1, std::move(error));
            return;
        }
        pending.callback(pending.instanceId, {});
    }

    MutableTimingPlugin* mutableInstance(int32_t instanceId) {
        const auto it = instances_.find(instanceId);
        return it == instances_.end()
            ? nullptr
            : dynamic_cast<MutableTimingPlugin*>(it->second.get());
    }

private:
    struct PendingCreation {
        int32_t instanceId{-1};
        std::function<void(int32_t, std::string)> callback;
    };

    int32_t next_instance_id_{100};
    std::map<int32_t, std::unique_ptr<MutableTimingPlugin>> instances_{};
    bool defer_creation_{false};
    bool defer_state_load_{false};
    std::vector<PendingCreation> pending_creations_{};
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

    std::optional<uapmd::ProjectUndoResult> moveHistory(
        uapmd::TimelineFacade& timeline,
        bool redo) {
        std::optional<uapmd::ProjectUndoResult> result;
        auto completion = [&result](uapmd::ProjectUndoResult completed) {
            result = std::move(completed);
        };
        if (redo)
            timeline.undoEngine().redo(std::move(completion));
        else
            timeline.undoEngine().undo(std::move(completion));
        return result;
    }
}

TEST_F(SequencerEngineOutputTest, ClipPropertyMutationsUndoAndRedoIndividually) {
    constexpr int32_t sampleRate = 48000;
    auto engine = uapmd::SequencerEngine::create(sampleRate, 256, 65536);
    ASSERT_NE(engine, nullptr);
    const auto trackIndex = engine->addEmptyTrack();
    ASSERT_GE(trackIndex, 0);
    auto& timeline = engine->timeline();
    const auto added = addFragmentTestClip(*engine, trackIndex, sampleRate);
    ASSERT_TRUE(added.success) << added.error;

    auto exercise = [&](auto mutate, auto verifyBefore, auto verifyAfter) {
        const auto before = timeline.captureClipFragment(trackIndex, added.clipId);
        ASSERT_TRUE(before.has_value());
        ASSERT_TRUE(mutate());
        const auto after = timeline.captureClipFragment(trackIndex, added.clipId);
        ASSERT_TRUE(after.has_value());

        const auto undone = moveHistory(timeline, false);
        ASSERT_TRUE(undone.has_value());
        ASSERT_TRUE(undone->succeeded()) << undone->error;
        const auto restored = timeline.captureClipFragment(trackIndex, added.clipId);
        ASSERT_TRUE(restored.has_value());
        verifyBefore(*before, *restored);

        const auto redone = moveHistory(timeline, true);
        ASSERT_TRUE(redone.has_value());
        ASSERT_TRUE(redone->succeeded()) << redone->error;
        const auto reapplied = timeline.captureClipFragment(trackIndex, added.clipId);
        ASSERT_TRUE(reapplied.has_value());
        verifyAfter(*after, *reapplied);
    };

    exercise(
        [&] { return timeline.setClipEnabled(trackIndex, added.clipId, false); },
        [](const auto& before, const auto& actual) {
            EXPECT_EQ(actual.clip.enabled, before.clip.enabled);
        },
        [](const auto& after, const auto& actual) {
            EXPECT_EQ(actual.clip.enabled, after.clip.enabled);
        });
    exercise(
        [&] {
            return timeline.setClipAnchor(
                trackIndex,
                added.clipId,
                uapmd::TimeReference::fromContainerEnd({}, 0.25));
        },
        [](const auto& before, const auto& actual) {
            EXPECT_EQ(actual.clip.timeReference(48000), before.clip.timeReference(48000));
        },
        [](const auto& after, const auto& actual) {
            EXPECT_EQ(actual.clip.timeReference(48000), after.clip.timeReference(48000));
        });
    exercise(
        [&] { return timeline.setClipGain(trackIndex, added.clipId, 0.25); },
        [](const auto& before, const auto& actual) {
            EXPECT_DOUBLE_EQ(actual.clip.gain, before.clip.gain);
        },
        [](const auto& after, const auto& actual) {
            EXPECT_DOUBLE_EQ(actual.clip.gain, after.clip.gain);
        });
    exercise(
        [&] { return timeline.setClipMuted(trackIndex, added.clipId, true); },
        [](const auto& before, const auto& actual) {
            EXPECT_EQ(actual.clip.muted, before.clip.muted);
        },
        [](const auto& after, const auto& actual) {
            EXPECT_EQ(actual.clip.muted, after.clip.muted);
        });
    exercise(
        [&] { return timeline.resizeClip(trackIndex, added.clipId, 12345); },
        [](const auto& before, const auto& actual) {
            EXPECT_EQ(actual.clip.durationSamples, before.clip.durationSamples);
        },
        [](const auto& after, const auto& actual) {
            EXPECT_EQ(actual.clip.durationSamples, after.clip.durationSamples);
        });
    exercise(
        [&] { return timeline.setClipName(trackIndex, added.clipId, "Renamed"); },
        [](const auto& before, const auto& actual) {
            EXPECT_EQ(actual.clip.name, before.clip.name);
        },
        [](const auto& after, const auto& actual) {
            EXPECT_EQ(actual.clip.name, after.clip.name);
        });
    exercise(
        [&] { return timeline.setClipFilepath(trackIndex, added.clipId, "edited.mid"); },
        [](const auto& before, const auto& actual) {
            EXPECT_EQ(actual.clip.filepath, before.clip.filepath);
        },
        [](const auto& after, const auto& actual) {
            EXPECT_EQ(actual.clip.filepath, after.clip.filepath);
        });
    exercise(
        [&] { return timeline.setClipNeedsFileSave(trackIndex, added.clipId, true); },
        [](const auto& before, const auto& actual) {
            EXPECT_EQ(actual.clip.needsFileSave, before.clip.needsFileSave);
        },
        [](const auto& after, const auto& actual) {
            EXPECT_EQ(actual.clip.needsFileSave, after.clip.needsFileSave);
        });

    const std::vector<uapmd::ClipMarker> markers{
        {"marker", 480, uapmd::AudioWarpReferenceType::ClipStart, {}, {}, "Marker"}};
    exercise(
        [&] { return timeline.setClipMarkers(trackIndex, added.clipId, markers); },
        [](const auto& before, const auto& actual) {
            EXPECT_EQ(actual.clip.markers.size(), before.clip.markers.size());
        },
        [](const auto& after, const auto& actual) {
            ASSERT_EQ(actual.clip.markers.size(), after.clip.markers.size());
            EXPECT_EQ(actual.clip.markers[0].markerId, after.clip.markers[0].markerId);
        });

    const std::vector<uapmd::AudioWarpPoint> warps{
        {0.0, 1.0, uapmd::AudioWarpReferenceType::ClipStart, {}, {}},
        {0.5, 0.75, uapmd::AudioWarpReferenceType::ClipStart, {}, {}},
    };
    exercise(
        [&] { return timeline.setClipAudioWarps(trackIndex, added.clipId, warps); },
        [](const auto& before, const auto& actual) {
            EXPECT_EQ(actual.clip.audioWarps.size(), before.clip.audioWarps.size());
        },
        [](const auto& after, const auto& actual) {
            ASSERT_EQ(actual.clip.audioWarps.size(), after.clip.audioWarps.size());
            EXPECT_DOUBLE_EQ(actual.clip.audioWarps[1].speedRatio, after.clip.audioWarps[1].speedRatio);
        });
}

TEST_F(SequencerEngineOutputTest, MasterTrackMarkersAreOwnedByTheEngine) {
    auto engine = uapmd::SequencerEngine::create(48000, 256, 65536);
    ASSERT_NE(engine, nullptr);

    std::vector<uapmd::ClipMarker> markers{
        {"engine-marker", 0.5, uapmd::AudioWarpReferenceType::ClipStart, {}, {}, "Marker"}};
    engine->setMasterTrackMarkers(markers);

    ASSERT_EQ(engine->masterTrackMarkers().size(), 1u);
    EXPECT_EQ(engine->masterTrackMarkers()[0].markerId, "engine-marker");
    EXPECT_DOUBLE_EQ(engine->masterTrackMarkers()[0].clipPositionOffset, 0.5);
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
    EXPECT_EQ(restored.clipId, added.clipId);
    EXPECT_EQ(restoredClip->referenceId, capturedReferenceId);
    EXPECT_EQ(restoredClip->name, "Renamed");

    const auto recaptured = timeline.captureClipFragment(trackIndex, restored.clipId);
    ASSERT_TRUE(recaptured.has_value());
    EXPECT_EQ(recaptured->umpEvents, kFragmentUmp);
}

TEST_F(SequencerEngineOutputTest, MidiClipContentUndoRestoresClipIdentityAndEvents) {
    constexpr int32_t sampleRate = 48000;
    auto engine = uapmd::SequencerEngine::create(sampleRate, 256, 65536);
    ASSERT_NE(engine, nullptr);
    const auto trackIndex = engine->addEmptyTrack();
    ASSERT_GE(trackIndex, 0);

    const auto added = addFragmentTestClip(*engine, trackIndex, sampleRate);
    ASSERT_TRUE(added.success) << added.error;
    auto& timeline = engine->timeline();
    const auto originalReferenceId =
        timeline.tracks()[static_cast<size_t>(trackIndex)]->clipManager()
            .getClip(added.clipId)->referenceId;

    const std::vector<uapmd_ump_t> replacementEvents{
        0x40903E00u, 0x7FFF0000u,
        0x40803E00u, 0x00000000u,
    };
    const std::vector<uint64_t> replacementTicks{0, 0, 960, 960};
    ASSERT_TRUE(timeline.replaceMidiClipContent(
        trackIndex, added.clipId, replacementEvents, replacementTicks));

    auto track = timeline.tracks()[static_cast<size_t>(trackIndex)];
    auto* changedClip = track->clipManager().getClip(added.clipId);
    ASSERT_NE(changedClip, nullptr);
    auto changedSource = track->getSourceNode(changedClip->sourceNodeInstanceId);
    ASSERT_NE(changedSource, nullptr);
    ASSERT_TRUE(std::dynamic_pointer_cast<uapmd::MidiClipSourceNode>(changedSource));
    EXPECT_EQ(
        std::dynamic_pointer_cast<uapmd::MidiClipSourceNode>(changedSource)->umpEvents(),
        replacementEvents);

    std::optional<uapmd::ProjectUndoResult> undoResult;
    timeline.undoEngine().undo([&undoResult](uapmd::ProjectUndoResult result) {
        undoResult = std::move(result);
    });
    ASSERT_TRUE(undoResult.has_value());
    ASSERT_TRUE(undoResult->succeeded()) << undoResult->error;

    auto* restoredClip = track->clipManager().getClip(added.clipId);
    ASSERT_NE(restoredClip, nullptr);
    EXPECT_EQ(restoredClip->referenceId, originalReferenceId);
    auto restoredSource = track->getSourceNode(restoredClip->sourceNodeInstanceId);
    ASSERT_NE(restoredSource, nullptr);
    auto restoredMidi = std::dynamic_pointer_cast<uapmd::MidiClipSourceNode>(restoredSource);
    ASSERT_NE(restoredMidi, nullptr);
    EXPECT_EQ(restoredMidi->umpEvents(), kFragmentUmp);
    EXPECT_EQ(restoredMidi->eventTimestampsTicks(), kFragmentTicks);

    std::optional<uapmd::ProjectUndoResult> redoResult;
    timeline.undoEngine().redo([&redoResult](uapmd::ProjectUndoResult result) {
        redoResult = std::move(result);
    });
    ASSERT_TRUE(redoResult.has_value());
    ASSERT_TRUE(redoResult->succeeded()) << redoResult->error;
    auto* redoneClip = track->clipManager().getClip(added.clipId);
    ASSERT_NE(redoneClip, nullptr);
    EXPECT_EQ(redoneClip->referenceId, originalReferenceId);
    auto redoneSource = track->getSourceNode(redoneClip->sourceNodeInstanceId);
    ASSERT_NE(redoneSource, nullptr);
    auto redoneMidi = std::dynamic_pointer_cast<uapmd::MidiClipSourceNode>(redoneSource);
    ASSERT_NE(redoneMidi, nullptr);
    EXPECT_EQ(redoneMidi->umpEvents(), replacementEvents);
    EXPECT_EQ(redoneMidi->eventTimestampsTicks(), replacementTicks);
}

TEST_F(SequencerEngineOutputTest, MidiEventAppendUndoAndRedo) {
    auto engine = uapmd::SequencerEngine::create(48000, 256, 65536);
    ASSERT_NE(engine, nullptr);
    const auto trackIndex = engine->addEmptyTrack();
    ASSERT_GE(trackIndex, 0);
    auto& timeline = engine->timeline();
    const auto added = addFragmentTestClip(*engine, trackIndex, 48000);
    ASSERT_TRUE(added.success) << added.error;
    auto* track = timeline.tracks()[static_cast<size_t>(trackIndex)];
    auto readSource = [&] {
        const auto* clip = track->clipManager().getClip(added.clipId);
        return std::dynamic_pointer_cast<uapmd::MidiClipSourceNode>(
            track->getSourceNode(clip->sourceNodeInstanceId));
    };
    auto source = readSource();
    ASSERT_NE(source, nullptr);
    const auto before = source->umpEvents();
    const std::vector<uapmd_ump_t> appended{
        0x40903F00u, 0x7FFF0000u,
    };
    ASSERT_TRUE(timeline.appendMidiEventsToClip(
        trackIndex,
        added.clipId,
        appended,
        {960, 960}));
    source = readSource();
    ASSERT_NE(source, nullptr);
    EXPECT_EQ(source->umpEvents().size(), before.size() + appended.size());
    auto result = moveHistory(timeline, false);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->succeeded()) << result->error;
    source = readSource();
    ASSERT_NE(source, nullptr);
    EXPECT_EQ(source->umpEvents(), before);
    result = moveHistory(timeline, true);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->succeeded()) << result->error;
    source = readSource();
    ASSERT_NE(source, nullptr);
    EXPECT_EQ(source->umpEvents().size(), before.size() + appended.size());
}

TEST_F(SequencerEngineOutputTest, ClipCreationDeletionAndClearUndoAndRedo) {
    ScopedTestEventLoop eventLoop;
    constexpr int32_t sampleRate = 48000;
    auto engine = uapmd::SequencerEngine::create(sampleRate, 256, 65536);
    ASSERT_NE(engine, nullptr);
    const auto trackIndex = engine->addEmptyTrack();
    ASSERT_GE(trackIndex, 0);
    auto& timeline = engine->timeline();

    const auto directlyAdded = addFragmentTestClip(*engine, trackIndex, sampleRate);
    ASSERT_TRUE(directlyAdded.success) << directlyAdded.error;
    EXPECT_EQ(timeline.tracks()[static_cast<size_t>(trackIndex)]->clipManager().clipCount(), 1u);
    auto result = moveHistory(timeline, false);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->succeeded()) << result->error;
    EXPECT_EQ(timeline.tracks()[static_cast<size_t>(trackIndex)]->clipManager().clipCount(), 0u);
    result = moveHistory(timeline, true);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->succeeded()) << result->error;
    EXPECT_NE(
        timeline.tracks()[static_cast<size_t>(trackIndex)]->clipManager().getClip(directlyAdded.clipId),
        nullptr);

    const auto first = addFragmentTestClip(*engine, trackIndex, sampleRate);
    const auto second = addFragmentTestClip(*engine, trackIndex, sampleRate);
    ASSERT_TRUE(first.success) << first.error;
    ASSERT_TRUE(second.success) << second.error;
    ASSERT_EQ(timeline.tracks()[static_cast<size_t>(trackIndex)]->clipManager().clipCount(), 3u);

    ASSERT_TRUE(timeline.removeClipFromTrack(trackIndex, first.clipId));
    EXPECT_EQ(timeline.tracks()[static_cast<size_t>(trackIndex)]->clipManager().clipCount(), 2u);
    result = moveHistory(timeline, false);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->succeeded()) << result->error;
    EXPECT_NE(
        timeline.tracks()[static_cast<size_t>(trackIndex)]->clipManager().getClip(first.clipId),
        nullptr);
    result = moveHistory(timeline, true);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->succeeded()) << result->error;
    EXPECT_EQ(
        timeline.tracks()[static_cast<size_t>(trackIndex)]->clipManager().getClip(first.clipId),
        nullptr);

    ASSERT_TRUE(timeline.clearClipsFromTrack(trackIndex));
    EXPECT_EQ(timeline.tracks()[static_cast<size_t>(trackIndex)]->clipManager().clipCount(), 0u);
    remidy::EventLoop::processQueuedTasks();
    result = moveHistory(timeline, false);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->succeeded()) << result->error;
    EXPECT_EQ(timeline.tracks()[static_cast<size_t>(trackIndex)]->clipManager().clipCount(), 2u);
    result = moveHistory(timeline, true);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->succeeded()) << result->error;
    EXPECT_EQ(timeline.tracks()[static_cast<size_t>(trackIndex)]->clipManager().clipCount(), 0u);
}

TEST_F(SequencerEngineOutputTest, TrackCreationAndDeletionUndoAndRedo) {
    ScopedTestEventLoop eventLoop;
    auto engine = uapmd::SequencerEngine::create(48000, 256, 65536);
    ASSERT_NE(engine, nullptr);
    auto& timeline = engine->timeline();

    auto waitForTrackMutation = [](std::optional<int32_t>& index) {
        for (int i = 0; i < 100 && !index.has_value(); ++i) {
            remidy::EventLoop::processQueuedTasks();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };

    std::optional<int32_t> addedIndex;
    std::string addError;
    timeline.addEmptyTrack(
        uapmd::ProjectMutationOrigin::User,
        [&](int32_t index, std::string error) {
            addedIndex = index;
            addError = std::move(error);
        });
    waitForTrackMutation(addedIndex);
    ASSERT_TRUE(addedIndex.has_value()) << addError;
    ASSERT_GE(*addedIndex, 0);
    const auto trackCountAfterAdd = engine->tracks().size();
    ASSERT_GT(trackCountAfterAdd, 0u);

    auto result = moveHistory(timeline, false);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->succeeded()) << result->error;
    EXPECT_EQ(engine->tracks().size(), trackCountAfterAdd - 1);
    result = moveHistory(timeline, true);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->succeeded()) << result->error;
    EXPECT_EQ(engine->tracks().size(), trackCountAfterAdd);

    auto engineForRemoval = uapmd::SequencerEngine::create(48000, 256, 65536);
    ASSERT_NE(engineForRemoval, nullptr);
    const auto removable = engineForRemoval->addEmptyTrack();
    ASSERT_GE(removable, 0);
    auto& removalTimeline = engineForRemoval->timeline();
    std::optional<int32_t> removedIndex;
    std::string removeError;
    removalTimeline.removeTrack(
        removable,
        uapmd::ProjectMutationOrigin::User,
        [&](int32_t index, std::string error) {
            removedIndex = index;
            removeError = std::move(error);
        });
    waitForTrackMutation(removedIndex);
    ASSERT_TRUE(removedIndex.has_value()) << removeError;
    ASSERT_GE(*removedIndex, 0);
    EXPECT_EQ(engineForRemoval->tracks().size(), 0u);
    result = moveHistory(removalTimeline, false);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->succeeded()) << result->error;
    EXPECT_EQ(engineForRemoval->tracks().size(), 1u);
    result = moveHistory(removalTimeline, true);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->succeeded()) << result->error;
    EXPECT_EQ(engineForRemoval->tracks().size(), 0u);
}

TEST_F(SequencerEngineOutputTest, GraphTypeAndConnectionUndoAndRedo) {
    auto engine = uapmd::SequencerEngine::create(48000, 256, 65536);
    ASSERT_NE(engine, nullptr);
    const auto trackIndex = engine->addEmptyTrack();
    ASSERT_GE(trackIndex, 0);
    auto& timeline = engine->timeline();
    ASSERT_TRUE(timeline.replaceTrackGraphType(
        trackIndex,
        "urn:uapmd-graph:common/graph/dag/v1",
        engine->umpBufferSizeInBytes()));
    auto* graph = dynamic_cast<uapmd_graph::AudioPluginFullDAGraph*>(
        &engine->tracks()[static_cast<size_t>(trackIndex)]->graph());
    ASSERT_NE(graph, nullptr);
    // The graph-type migration establishes the default simple topology. Clear
    // that setup topology so this test isolates the authored connection step.
    graph->clearConnections();

    uapmd_graph::AudioPluginGraphConnection connection;
    connection.source.type = uapmd_graph::AudioPluginGraphEndpointType::GraphInput;
    connection.target.type = uapmd_graph::AudioPluginGraphEndpointType::GraphOutput;
    std::string error;
    ASSERT_TRUE(timeline.connectTrackGraph(trackIndex, connection, error)) << error;
    ASSERT_EQ(graph->connections().size(), 1u);

    auto result = moveHistory(timeline, false);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->succeeded()) << result->error;
    EXPECT_TRUE(graph->connections().empty());
    result = moveHistory(timeline, true);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->succeeded()) << result->error;
    ASSERT_EQ(graph->connections().size(), 1u);
    const auto connectionId = graph->connections().front().id;

    ASSERT_TRUE(timeline.disconnectTrackGraphConnection(trackIndex, connectionId, error)) << error;
    EXPECT_TRUE(graph->connections().empty());
    result = moveHistory(timeline, false);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->succeeded()) << result->error;
    EXPECT_EQ(graph->connections().size(), 1u);
    result = moveHistory(timeline, true);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->succeeded()) << result->error;
    EXPECT_TRUE(graph->connections().empty());

    // The graph-type replacement itself is also a history item beneath the
    // connection edits. Undoing the three authored steps returns to the
    // simple graph, and redoing restores the full-DAG graph.
    result = moveHistory(timeline, false);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->succeeded()) << result->error;
    result = moveHistory(timeline, false);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->succeeded()) << result->error;
    result = moveHistory(timeline, false);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->succeeded()) << result->error;
    EXPECT_EQ(
        dynamic_cast<uapmd_graph::AudioPluginFullDAGraph*>(
            &engine->tracks()[static_cast<size_t>(trackIndex)]->graph()),
        nullptr);
    result = moveHistory(timeline, true);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->succeeded()) << result->error;
    EXPECT_NE(
        dynamic_cast<uapmd_graph::AudioPluginFullDAGraph*>(
            &engine->tracks()[static_cast<size_t>(trackIndex)]->graph()),
        nullptr);
}

TEST_F(SequencerEngineOutputTest, DeviceInputCreationChangeAndRemovalUndoAndRedo) {
    auto engine = uapmd::SequencerEngine::create(48000, 256, 65536);
    ASSERT_NE(engine, nullptr);
    const auto trackIndex = engine->addEmptyTrack();
    ASSERT_GE(trackIndex, 0);
    auto& timeline = engine->timeline();
    constexpr int32_t sourceNodeId = 7123;
    ASSERT_TRUE(timeline.addDeviceInputToTrack(trackIndex, sourceNodeId, {0, 1}));
    ASSERT_TRUE(timeline.setDeviceInputChannels(trackIndex, sourceNodeId, {2}));
    auto* track = timeline.tracks()[static_cast<size_t>(trackIndex)];
    auto input = std::dynamic_pointer_cast<uapmd::DeviceInputSourceNode>(
        track->getSourceNode(sourceNodeId));
    ASSERT_NE(input, nullptr);
    EXPECT_EQ(input->getInputChannels(), (std::vector<uint32_t>{2}));

    auto result = moveHistory(timeline, false);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->succeeded()) << result->error;
    input = std::dynamic_pointer_cast<uapmd::DeviceInputSourceNode>(
        track->getSourceNode(sourceNodeId));
    ASSERT_NE(input, nullptr);
    EXPECT_EQ(input->getInputChannels(), (std::vector<uint32_t>{0, 1}));
    result = moveHistory(timeline, true);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->succeeded()) << result->error;

    ASSERT_TRUE(timeline.removeDeviceInputFromTrack(trackIndex, sourceNodeId));
    EXPECT_EQ(track->getSourceNode(sourceNodeId), nullptr);
    result = moveHistory(timeline, false);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->succeeded()) << result->error;
    EXPECT_NE(track->getSourceNode(sourceNodeId), nullptr);
    result = moveHistory(timeline, true);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->succeeded()) << result->error;
    EXPECT_EQ(track->getSourceNode(sourceNodeId), nullptr);
}

TEST_F(SequencerEngineOutputTest, PluginPropertiesStateAndLifecycleUndoAndRedo) {
    ScopedTestEventLoop eventLoop;
    auto engine = uapmd::SequencerEngine::createWithPluginHost(
        48000,
        256,
        65536,
        std::make_unique<TestPluginHostingAPI>());
    ASSERT_NE(engine, nullptr);
    const auto trackIndex = engine->addEmptyTrack();
    ASSERT_GE(trackIndex, 0);
    auto& timeline = engine->timeline();

    std::string format = "Test";
    std::string pluginId = "test.plugin";
    std::optional<int32_t> instanceId;
    std::string addError;
    engine->addPluginToTrack(
        trackIndex,
        format,
        pluginId,
        [&](int32_t id, int32_t, std::string error) {
            instanceId = id;
            addError = std::move(error);
        });
    ASSERT_TRUE(instanceId.has_value()) << addError;
    auto* plugin = dynamic_cast<MutableTimingPlugin*>(engine->getPluginInstance(*instanceId));
    ASSERT_NE(plugin, nullptr);

    auto drain = [] {
        for (int i = 0; i < 100; ++i) {
            remidy::EventLoop::processQueuedTasks();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    auto moveAndDrain = [&](bool redo) {
        std::optional<uapmd::ProjectUndoResult> result;
        if (redo)
            timeline.undoEngine().redo([&result](auto completed) { result = std::move(completed); });
        else
            timeline.undoEngine().undo([&result](auto completed) { result = std::move(completed); });
        drain();
        return result;
    };

    ASSERT_TRUE(timeline.undoEngine().clear());
    plugin->externallySetParameter(2, 0.2);
    plugin->externallySetParameter(2, 0.8);
    drain();
    EXPECT_DOUBLE_EQ(plugin->getParameterValue(2), 0.8);
    auto result = moveAndDrain(false);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->succeeded()) << result->error;
    EXPECT_DOUBLE_EQ(plugin->getParameterValue(2), 0.2);
    result = moveAndDrain(true);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->succeeded()) << result->error;
    EXPECT_DOUBLE_EQ(plugin->getParameterValue(2), 0.8);

    ASSERT_TRUE(timeline.setPluginBypassed(*instanceId, true));
    EXPECT_TRUE(plugin->bypassed());
    result = moveAndDrain(false);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->succeeded()) << result->error;
    EXPECT_FALSE(plugin->bypassed());
    result = moveAndDrain(true);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->succeeded()) << result->error;
    EXPECT_TRUE(plugin->bypassed());

    ASSERT_TRUE(timeline.setPluginParameterValue(*instanceId, 4, 0.75));
    EXPECT_DOUBLE_EQ(plugin->getParameterValue(4), 0.75);
    result = moveAndDrain(false);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->succeeded()) << result->error;
    EXPECT_DOUBLE_EQ(plugin->getParameterValue(4), 0.0);
    result = moveAndDrain(true);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->succeeded()) << result->error;
    EXPECT_DOUBLE_EQ(plugin->getParameterValue(4), 0.75);

    const remidy::PerNoteControllerContext noteContext{60, 1, 2, 0};
    ASSERT_TRUE(timeline.setPluginPerNoteControllerValue(
        *instanceId,
        remidy::PER_NOTE_CONTROLLER_PER_NOTE,
        noteContext,
        7,
        0.6));
    double noteValue = 0.0;
    ASSERT_TRUE(plugin->getPerNoteControllerValue(60, 7, &noteValue));
    EXPECT_DOUBLE_EQ(noteValue, 0.6);
    result = moveAndDrain(false);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->succeeded()) << result->error;
    ASSERT_TRUE(plugin->getPerNoteControllerValue(60, 7, &noteValue));
    EXPECT_DOUBLE_EQ(noteValue, 0.0);
    result = moveAndDrain(true);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->succeeded()) << result->error;
    ASSERT_TRUE(plugin->getPerNoteControllerValue(60, 7, &noteValue));
    EXPECT_DOUBLE_EQ(noteValue, 0.6);

    ASSERT_TRUE(timeline.setPluginGroup(*instanceId, 3));
    EXPECT_EQ(engine->getInstanceGroup(*instanceId), 3u);
    result = moveAndDrain(false);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->succeeded()) << result->error;
    EXPECT_EQ(engine->getInstanceGroup(*instanceId), 0u);
    result = moveAndDrain(true);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->succeeded()) << result->error;
    EXPECT_EQ(engine->getInstanceGroup(*instanceId), 3u);

    std::optional<uapmd::ProjectUndoResult> stateResult;
    timeline.setPluginState(
        *instanceId,
        {1, 2, 3},
        uapmd::ProjectMutationOrigin::User,
        [&stateResult](uapmd::ProjectUndoResult completed) {
            stateResult = std::move(completed);
        });
    drain();
    ASSERT_TRUE(stateResult.has_value());
    ASSERT_TRUE(stateResult->succeeded()) << stateResult->error;
    EXPECT_EQ(plugin->saveStateSync(), (std::vector<uint8_t>{1, 2, 3}));
    stateResult = moveAndDrain(false);
    ASSERT_TRUE(stateResult.has_value());
    ASSERT_TRUE(stateResult->succeeded()) << stateResult->error;
    EXPECT_TRUE(plugin->saveStateSync().empty());
    stateResult = moveAndDrain(true);
    ASSERT_TRUE(stateResult.has_value());
    ASSERT_TRUE(stateResult->succeeded()) << stateResult->error;
    EXPECT_EQ(plugin->saveStateSync(), (std::vector<uint8_t>{1, 2, 3}));

    ASSERT_TRUE(timeline.undoEngine().clear());
    std::optional<uapmd::ProjectUndoResult> lifecycleResult;
    timeline.recordPluginInstanceAddition(
        *instanceId,
        uapmd::ProjectMutationOrigin::User,
        [&lifecycleResult](uapmd::ProjectUndoResult completed) {
            lifecycleResult = std::move(completed);
        });
    drain();
    ASSERT_TRUE(lifecycleResult.has_value());
    ASSERT_TRUE(lifecycleResult->succeeded()) << lifecycleResult->error;
    lifecycleResult = moveAndDrain(false);
    ASSERT_TRUE(lifecycleResult.has_value());
    ASSERT_TRUE(lifecycleResult->succeeded()) << lifecycleResult->error;
    EXPECT_EQ(engine->findTrackIndexForInstance(*instanceId), -1);
    lifecycleResult = moveAndDrain(true);
    ASSERT_TRUE(lifecycleResult.has_value());
    ASSERT_TRUE(lifecycleResult->succeeded()) << lifecycleResult->error;
    ASSERT_EQ(engine->tracks()[static_cast<size_t>(trackIndex)]->orderedInstanceIds().size(), 1u);

    const auto restoredInstanceId =
        engine->tracks()[static_cast<size_t>(trackIndex)]->orderedInstanceIds().front();
    ASSERT_TRUE(timeline.undoEngine().clear());
    lifecycleResult.reset();
    timeline.removePluginInstance(
        restoredInstanceId,
        uapmd::ProjectMutationOrigin::User,
        [&lifecycleResult](uapmd::ProjectUndoResult completed) {
            lifecycleResult = std::move(completed);
        });
    drain();
    ASSERT_TRUE(lifecycleResult.has_value());
    ASSERT_TRUE(lifecycleResult->succeeded()) << lifecycleResult->error;
    EXPECT_TRUE(engine->tracks()[static_cast<size_t>(trackIndex)]->orderedInstanceIds().empty());
    lifecycleResult = moveAndDrain(false);
    ASSERT_TRUE(lifecycleResult.has_value());
    ASSERT_TRUE(lifecycleResult->succeeded()) << lifecycleResult->error;
    EXPECT_EQ(engine->tracks()[static_cast<size_t>(trackIndex)]->orderedInstanceIds().size(), 1u);
    lifecycleResult = moveAndDrain(true);
    ASSERT_TRUE(lifecycleResult.has_value());
    ASSERT_TRUE(lifecycleResult->succeeded()) << lifecycleResult->error;
    EXPECT_TRUE(engine->tracks()[static_cast<size_t>(trackIndex)]->orderedInstanceIds().empty());
}

TEST_F(SequencerEngineOutputTest, AudioClipContentUndoRestoresSourceAndMetadata) {
    constexpr int32_t sampleRate = 48000;
    constexpr uint64_t frameCount = 480;
    const auto audioPath = test_dir_ / "undo-audio.wav";
    choc::audio::AudioFileProperties properties;
    properties.sampleRate = sampleRate;
    properties.numChannels = 2;
    properties.numFrames = frameCount;
    properties.bitDepth = choc::audio::BitDepth::float32;
    auto writer = choc::audio::WAVAudioFileFormat<true>().createWriter(
        audioPath.string(), properties);
    ASSERT_NE(writer, nullptr);
    choc::buffer::ChannelArrayBuffer<float> audioBuffer(2, frameCount);
    for (uint32_t channel = 0; channel < 2; ++channel)
        for (uint32_t frame = 0; frame < frameCount; ++frame)
            audioBuffer.getSample(channel, frame) =
                channel == 0 ? 0.1f : -0.1f;
    ASSERT_TRUE(writer->appendFrames(audioBuffer.getView()));
    ASSERT_TRUE(writer->flush());

    auto engine = uapmd::SequencerEngine::create(sampleRate, 256, 65536);
    ASSERT_NE(engine, nullptr);
    const auto trackIndex = engine->addEmptyTrack();
    ASSERT_GE(trackIndex, 0);
    auto& timeline = engine->timeline();
    auto added = timeline.addAudioClipToTrack(
        trackIndex,
        uapmd::TimelinePosition::fromSamples(0, sampleRate),
        std::make_unique<SineAudioFileReader>(frameCount, 2, sampleRate, 440.0, 0.2f),
        audioPath.string());
    ASSERT_TRUE(added.success) << added.error;

    const std::vector<uapmd::ClipMarker> markers{
        {"phrase", 120, uapmd::AudioWarpReferenceType::ClipStart, {}, {}, "Phrase"}};
    const std::vector<uapmd::AudioWarpPoint> warps{
        {0.0, 1.0, uapmd::AudioWarpReferenceType::ClipStart, {}, {}},
        {0.25, 0.5, uapmd::AudioWarpReferenceType::ClipStart, {}, {}},
    };
    ASSERT_TRUE(timeline.replaceAudioClipContent(
        trackIndex,
        added.clipId,
        audioPath.string(),
        markers,
        warps,
        {}));
    auto changed = timeline.captureClipFragment(trackIndex, added.clipId);
    ASSERT_TRUE(changed.has_value());
    ASSERT_EQ(changed->clip.markers.size(), 1u);
    ASSERT_EQ(changed->clip.audioWarps.size(), 2u);

    auto result = moveHistory(timeline, false);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->succeeded()) << result->error;
    auto restored = timeline.captureClipFragment(trackIndex, added.clipId);
    ASSERT_TRUE(restored.has_value());
    EXPECT_TRUE(restored->clip.markers.empty());
    EXPECT_TRUE(restored->clip.audioWarps.empty());

    result = moveHistory(timeline, true);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->succeeded()) << result->error;
    auto redone = timeline.captureClipFragment(trackIndex, added.clipId);
    ASSERT_TRUE(redone.has_value());
    EXPECT_EQ(redone->clip.markers.size(), 1u);
    EXPECT_EQ(redone->clip.audioWarps.size(), 2u);
}

TEST_F(SequencerEngineOutputTest, TrackPropertiesAndDeviceRoutingUndoAndRedo) {
    auto engine = uapmd::SequencerEngine::create(48000, 256, 65536);
    ASSERT_NE(engine, nullptr);
    const auto firstTrack = engine->addEmptyTrack();
    const auto secondTrack = engine->addEmptyTrack();
    ASSERT_GE(firstTrack, 0);
    ASSERT_GE(secondTrack, 0);

    auto& timeline = engine->timeline();
    ASSERT_TRUE(timeline.setTrackGain(firstTrack, 0.5));
    ASSERT_TRUE(timeline.undoEngine().state().canUndo);
    std::optional<uapmd::ProjectUndoResult> result;
    timeline.undoEngine().undo([&result](uapmd::ProjectUndoResult completed) {
        result = std::move(completed);
    });
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->succeeded()) << result->error;
    EXPECT_DOUBLE_EQ(engine->tracks()[static_cast<size_t>(firstTrack)]->trackGain(), 1.0);
    timeline.undoEngine().redo([&result](uapmd::ProjectUndoResult completed) {
        result = std::move(completed);
    });
    ASSERT_TRUE(result->succeeded()) << result->error;
    EXPECT_DOUBLE_EQ(engine->tracks()[static_cast<size_t>(firstTrack)]->trackGain(), 0.5);

    ASSERT_TRUE(timeline.setTrackMuted(firstTrack, true));
    timeline.undoEngine().undo([&result](uapmd::ProjectUndoResult completed) {
        result = std::move(completed);
    });
    ASSERT_TRUE(result->succeeded()) << result->error;
    EXPECT_FALSE(engine->tracks()[static_cast<size_t>(firstTrack)]->muted());

    ASSERT_TRUE(timeline.setTrackBypassed(firstTrack, true));
    timeline.undoEngine().undo([&result](uapmd::ProjectUndoResult completed) {
        result = std::move(completed);
    });
    ASSERT_TRUE(result->succeeded()) << result->error;
    EXPECT_FALSE(engine->tracks()[static_cast<size_t>(firstTrack)]->bypassed());

    ASSERT_TRUE(timeline.setTrackFreezePolicyEnabled(firstTrack, true));
    EXPECT_EQ(
        engine->frozenTrackManager().freezePolicyForTrack(firstTrack),
        uapmd::FrozenTrackManager::FreezePolicy::On);
    timeline.undoEngine().undo([&result](uapmd::ProjectUndoResult completed) {
        result = std::move(completed);
    });
    ASSERT_TRUE(result->succeeded()) << result->error;
    EXPECT_EQ(
        engine->frozenTrackManager().freezePolicyForTrack(firstTrack),
        uapmd::FrozenTrackManager::FreezePolicy::Off);

    auto latencySettings = engine->latencyCompensationManager()->projectSettings();
    latencySettings.playback_compensation_mode =
        uapmd::PlaybackCompensationMode::LOW_LATENCY;
    latencySettings.input_monitoring_policy = uapmd::InputMonitoringPolicy::OFF;
    latencySettings.record_armed_track_indexes = {firstTrack};
    latencySettings.monitored_track_indexes = {secondTrack};
    ASSERT_TRUE(timeline.setLatencyCompensationSettings(latencySettings));
    timeline.undoEngine().undo([&result](uapmd::ProjectUndoResult completed) {
        result = std::move(completed);
    });
    ASSERT_TRUE(result->succeeded()) << result->error;
    const auto restoredLatencySettings =
        engine->latencyCompensationManager()->projectSettings();
    EXPECT_NE(
        restoredLatencySettings.playback_compensation_mode,
        uapmd::PlaybackCompensationMode::LOW_LATENCY);
    EXPECT_NE(
        restoredLatencySettings.input_monitoring_policy,
        uapmd::InputMonitoringPolicy::OFF);
    EXPECT_TRUE(restoredLatencySettings.record_armed_track_indexes.empty());
    EXPECT_TRUE(restoredLatencySettings.monitored_track_indexes.empty());

    const auto clip = addFragmentTestClip(*engine, firstTrack, 48000);
    ASSERT_TRUE(clip.success) << clip.error;
    EXPECT_FALSE(timeline.addDeviceInputToTrack(firstTrack, clip.sourceNodeId, {0, 1}));

    constexpr int32_t sourceNodeId = 9001;
    ASSERT_TRUE(timeline.addDeviceInputToTrack(firstTrack, sourceNodeId, {2, 3}));
    auto track = timeline.tracks()[static_cast<size_t>(firstTrack)];
    auto input = std::dynamic_pointer_cast<uapmd::DeviceInputSourceNode>(
        track->getSourceNode(sourceNodeId));
    ASSERT_NE(input, nullptr);
    EXPECT_EQ(input->getInputChannels(), (std::vector<uint32_t>{2, 3}));

    ASSERT_TRUE(timeline.setDeviceInputChannels(firstTrack, sourceNodeId, {4}));
    timeline.undoEngine().undo([&result](uapmd::ProjectUndoResult completed) {
        result = std::move(completed);
    });
    ASSERT_TRUE(result->succeeded()) << result->error;
    input = std::dynamic_pointer_cast<uapmd::DeviceInputSourceNode>(
        track->getSourceNode(sourceNodeId));
    ASSERT_NE(input, nullptr);
    EXPECT_EQ(input->getInputChannels(), (std::vector<uint32_t>{2, 3}));

    timeline.undoEngine().undo([&result](uapmd::ProjectUndoResult completed) {
        result = std::move(completed);
    });
    ASSERT_TRUE(result->succeeded()) << result->error;
    EXPECT_EQ(track->getSourceNode(sourceNodeId), nullptr);
    timeline.undoEngine().redo([&result](uapmd::ProjectUndoResult completed) {
        result = std::move(completed);
    });
    ASSERT_TRUE(result->succeeded()) << result->error;
    EXPECT_NE(track->getSourceNode(sourceNodeId), nullptr);

    ASSERT_TRUE(timeline.setTrackSolo(firstTrack, true));
    ASSERT_TRUE(timeline.setTrackSolo(secondTrack, true));
    timeline.undoEngine().undo([&result](uapmd::ProjectUndoResult completed) {
        result = std::move(completed);
    });
    ASSERT_TRUE(result->succeeded()) << result->error;
    EXPECT_TRUE(engine->tracks()[static_cast<size_t>(firstTrack)]->solo());
    EXPECT_FALSE(engine->tracks()[static_cast<size_t>(secondTrack)]->solo());
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

TEST_F(SequencerEngineOutputTest, TrackFragmentStaysDetachedDuringAsyncPluginRestoration) {
    ScopedTestEventLoop eventLoop;
    auto pluginHost = std::make_unique<TestPluginHostingAPI>(true, true);
    auto* pluginHostObserver = pluginHost.get();
    auto engine = uapmd::SequencerEngine::createWithPluginHost(
        48000,
        256,
        65536,
        std::move(pluginHost));
    ASSERT_NE(engine, nullptr);
    auto& timeline = engine->timeline();

    uapmd::ProjectTrackFragment fragment;
    fragment.referenceId = "track_async_restore";
    fragment.volume = 0.4;
    fragment.muted = true;
    uapmd::ProjectTrackPluginFragment plugin;
    plugin.nodeId = "plugin-node-persistent";
    plugin.pluginId = "test.plugin";
    plugin.format = "Test";
    plugin.displayName = "Test Plugin";
    plugin.groupIndex = 5;
    plugin.state = {1, 3, 5, 7};
    fragment.plugins.push_back(std::move(plugin));

    uapmd::ProjectTrackAttachOptions options;
    options.idPolicy = uapmd::ProjectObjectIdPolicy::Restore;
    std::optional<int32_t> attachedIndex;
    std::string attachError;
    timeline.attachTrackFragment(
        fragment,
        options,
        [&](int32_t trackIndex, std::string error) {
            attachedIndex = trackIndex;
            attachError = std::move(error);
        });

    ASSERT_TRUE(pluginHostObserver->creationPending());
    EXPECT_TRUE(engine->tracks().empty());
    EXPECT_TRUE(timeline.tracks().empty());
    EXPECT_FALSE(attachedIndex.has_value());

    pluginHostObserver->completeNextCreation();
    auto instanceIds = pluginHostObserver->instanceIds();
    ASSERT_EQ(instanceIds.size(), 1u);
    auto* restoredPlugin = pluginHostObserver->mutableInstance(instanceIds.front());
    ASSERT_NE(restoredPlugin, nullptr);
    ASSERT_TRUE(restoredPlugin->stateLoadPending());
    EXPECT_TRUE(engine->tracks().empty());
    EXPECT_TRUE(timeline.tracks().empty());
    EXPECT_EQ(engine->getPluginInstance(instanceIds.front()), nullptr);
    EXPECT_FALSE(attachedIndex.has_value());

    restoredPlugin->completeStateLoad();
    ASSERT_TRUE(attachedIndex.has_value()) << attachError;
    ASSERT_GE(*attachedIndex, 0) << attachError;
    ASSERT_EQ(engine->tracks().size(), 1u);
    ASSERT_EQ(timeline.tracks().size(), 1u);
    EXPECT_EQ(timeline.tracks().front()->referenceId(), fragment.referenceId);
    EXPECT_DOUBLE_EQ(engine->tracks().front()->trackGain(), 0.4);
    EXPECT_TRUE(engine->tracks().front()->muted());
    EXPECT_EQ(engine->getInstanceGroup(instanceIds.front()), 5u);
    EXPECT_EQ(engine->getPluginInstance(instanceIds.front()), restoredPlugin);
    EXPECT_EQ(restoredPlugin->saveStateSync(), fragment.plugins.front().state);
}

TEST_F(SequencerEngineOutputTest, FailedAsyncPluginStateRestoreNeverPublishesTrack) {
    ScopedTestEventLoop eventLoop;
    auto pluginHost = std::make_unique<TestPluginHostingAPI>(true, true);
    auto* pluginHostObserver = pluginHost.get();
    auto engine = uapmd::SequencerEngine::createWithPluginHost(
        48000,
        256,
        65536,
        std::move(pluginHost));
    ASSERT_NE(engine, nullptr);

    uapmd::ProjectTrackFragment fragment;
    fragment.referenceId = "track_failed_restore";
    uapmd::ProjectTrackPluginFragment plugin;
    plugin.pluginId = "test.plugin";
    plugin.format = "Test";
    plugin.state = {2, 4, 6};
    fragment.plugins.push_back(std::move(plugin));

    std::optional<int32_t> attachedIndex;
    std::string attachError;
    engine->timeline().attachTrackFragment(
        fragment,
        {},
        [&](int32_t trackIndex, std::string error) {
            attachedIndex = trackIndex;
            attachError = std::move(error);
        });
    ASSERT_TRUE(pluginHostObserver->creationPending());
    pluginHostObserver->completeNextCreation();
    const auto instanceIds = pluginHostObserver->instanceIds();
    ASSERT_EQ(instanceIds.size(), 1u);
    auto* restoredPlugin = pluginHostObserver->mutableInstance(instanceIds.front());
    ASSERT_NE(restoredPlugin, nullptr);
    ASSERT_TRUE(restoredPlugin->stateLoadPending());

    restoredPlugin->completeStateLoad("state rejected");
    ASSERT_TRUE(attachedIndex.has_value());
    EXPECT_EQ(*attachedIndex, -1);
    EXPECT_NE(attachError.find("state rejected"), std::string::npos);
    EXPECT_TRUE(engine->tracks().empty());
    EXPECT_TRUE(engine->timeline().tracks().empty());
    EXPECT_TRUE(pluginHostObserver->instanceIds().empty());
}

TEST_F(SequencerEngineOutputTest, PreparedPluginTrackAdditionIsOneUndoStep) {
    ScopedTestEventLoop eventLoop;
    auto engine = uapmd::SequencerEngine::createWithPluginHost(
        48000,
        256,
        65536,
        std::make_unique<TestPluginHostingAPI>());
    ASSERT_NE(engine, nullptr);

    auto prepared = engine->prepareTrack();
    ASSERT_NE(prepared, nullptr);
    std::string format = "Test";
    std::string pluginId = "test.plugin";
    std::optional<int32_t> instanceId;
    std::string pluginError;
    engine->addPluginToPreparedTrack(
        *prepared,
        format,
        pluginId,
        [&](int32_t restoredInstanceId, std::string error) {
            instanceId = restoredInstanceId;
            pluginError = std::move(error);
        });
    ASSERT_TRUE(instanceId.has_value()) << pluginError;

    const auto trackIndex = engine->publishPreparedTrack(std::move(prepared));
    ASSERT_GE(trackIndex, 0);
    ASSERT_EQ(engine->tracks().size(), 1u);
    ASSERT_EQ(engine->tracks().front()->orderedInstanceIds().size(), 1u);

    std::optional<int32_t> recordedIndex;
    std::string recordError;
    auto& timeline = engine->timeline();
    timeline.recordTrackAddition(
        trackIndex,
        uapmd::ProjectMutationOrigin::User,
        [&](int32_t completedTrackIndex, std::string error) {
            recordedIndex = completedTrackIndex;
            recordError = std::move(error);
        });
    ASSERT_TRUE(recordedIndex.has_value()) << recordError;
    ASSERT_EQ(*recordedIndex, trackIndex) << recordError;

    auto result = moveHistory(timeline, false);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->succeeded()) << result->error;
    EXPECT_TRUE(engine->tracks().empty());
    EXPECT_FALSE(timeline.undoEngine().state().canUndo);

    result = moveHistory(timeline, true);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->succeeded()) << result->error;
    ASSERT_EQ(engine->tracks().size(), 1u);
    EXPECT_EQ(engine->tracks().front()->orderedInstanceIds().size(), 1u);
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

    const auto generationBeforeGraphChange =
        frozen.invalidationGenerationForTrack(trackIndex);
    ASSERT_TRUE(timeline.replaceTrackGraphType(
        trackIndex,
        "urn:uapmd-graph:common/graph/dag/v1",
        engine->umpBufferSizeInBytes()));
    EXPECT_GT(
        frozen.invalidationGenerationForTrack(trackIndex),
        generationBeforeGraphChange);
}

TEST_F(SequencerEngineOutputTest, PluginRemovalNotifiesListenersWithoutHoldingInstanceMapLock) {
    auto engine = uapmd::SequencerEngine::create(48000, 256, 65536);
    ASSERT_NE(engine, nullptr);

    class InspectingLifecycleListener final : public uapmd::PluginInstanceLifecycleListener {
    public:
        explicit InspectingLifecycleListener(uapmd::SequencerEngine& engine)
            : engine_(engine) {
        }

        void pluginInstanceWillBeDestroyed(int32_t instanceId) override {
            observedInstance_ = engine_.getPluginInstance(instanceId);
        }

        uapmd_plugin_hosting::AudioPluginInstanceAPI* observedInstance_{nullptr};

    private:
        uapmd::SequencerEngine& engine_;
    } listener(*engine);

    engine->addPluginInstanceLifecycleListener(listener);
    // An unknown ID still exercises the lifecycle notification path, without
    // requiring a platform plug-in to be installed. The listener deliberately
    // calls back into getPluginInstance(); removePluginInstance() must not hold
    // instance_map_mutex_ while sending that notification.
    EXPECT_FALSE(engine->removePluginInstance(123456));
    engine->removePluginInstanceLifecycleListener(listener);
    EXPECT_EQ(listener.observedInstance_, nullptr);
}

} // namespace
