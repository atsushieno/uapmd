#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <choc/text/choc_JSON.h>

#include <uapmd-engine/uapmd-engine.hpp>
#include <uapmd-app-model/detail/AppModel.hpp>
#include <uapmd-app-model/detail/UapmdJSRuntime.hpp>

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
            tasks.swap(tasks_);
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

void pumpUntil(const std::function<bool()>& done) {
    for (int i = 0; i < 200 && !done(); ++i) {
        remidy::EventLoop::processQueuedTasks();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

std::optional<std::string> moveHistory(uapmd_app::AppModel& model, bool redo) {
    std::optional<std::string> error;
    auto completion = [&error](std::string completed) {
        error = std::move(completed);
    };
    if (redo)
        model.redo(std::move(completion));
    else
        model.undo(std::move(completion));
    pumpUntil([&error] { return error.has_value(); });
    return error;
}

} // namespace

TEST(AppLayerUndoIntegrationTest, MasterMarkersUndoAndRedo) {
    ScopedTestEventLoop eventLoop;
    auto* dispatcher = uapmd::defaultDeviceIODispatcher();
    uapmd_app::AppModel model(256, 65536, 48000, dispatcher);
    const std::vector<uapmd::ClipMarker> markers{
        {"master-marker", 0.5, uapmd::AudioWarpReferenceType::ClipStart, {}, {}, "Marker"}};
    std::string error;
    ASSERT_TRUE(model.setMasterTrackMarkersWithValidation(markers, error)) << error;
    ASSERT_EQ(model.sequencer().engine()->masterTrackMarkers().size(), 1u);

    auto result = moveHistory(model, false);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->empty()) << *result;
    EXPECT_TRUE(model.sequencer().engine()->masterTrackMarkers().empty());

    result = moveHistory(model, true);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->empty()) << *result;
    ASSERT_EQ(model.sequencer().engine()->masterTrackMarkers().size(), 1u);
    EXPECT_EQ(model.sequencer().engine()->masterTrackMarkers()[0].markerId, "master-marker");
}

TEST(AppLayerUndoIntegrationTest, TrackAddRemoveAndClearUndoAndRedo) {
    ScopedTestEventLoop eventLoop;
    auto* dispatcher = uapmd::defaultDeviceIODispatcher();
    uapmd_app::AppModel model(256, 65536, 48000, dispatcher);
    const auto initialTrackCount = model.trackCount();

    std::optional<int32_t> added;
    std::string addError;
    model.addTrack([&](int32_t index, std::string error) {
        added = index;
        addError = std::move(error);
    });
    pumpUntil([&] { return added.has_value(); });
    ASSERT_TRUE(added.has_value()) << addError;
    ASSERT_GE(*added, 0);
    ASSERT_EQ(model.trackCount(), initialTrackCount + 1);

    auto result = moveHistory(model, false);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->empty()) << *result;
    EXPECT_EQ(model.trackCount(), initialTrackCount);
    result = moveHistory(model, true);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->empty()) << *result;
    EXPECT_EQ(model.trackCount(), initialTrackCount + 1);

    std::optional<int32_t> removed;
    std::string removeError;
    model.removeTrack(*added, [&](int32_t index, std::string error) {
        removed = index;
        removeError = std::move(error);
    });
    pumpUntil([&] { return removed.has_value(); });
    ASSERT_TRUE(removed.has_value()) << removeError;
    ASSERT_GE(*removed, 0);
    EXPECT_EQ(model.trackCount(), initialTrackCount);
    result = moveHistory(model, false);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->empty()) << *result;
    EXPECT_EQ(model.trackCount(), initialTrackCount + 1);
    result = moveHistory(model, true);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->empty()) << *result;
    EXPECT_EQ(model.trackCount(), initialTrackCount);

    for (int i = 0; i < 2; ++i) {
        std::optional<int32_t> index;
        model.addTrack([&index](int32_t addedIndex, std::string) {
            index = addedIndex;
        });
        pumpUntil([&] { return index.has_value(); });
        ASSERT_TRUE(index.has_value());
        ASSERT_GE(*index, 0);
    }
    ASSERT_EQ(model.trackCount(), initialTrackCount + 2);

    std::optional<std::string> clearError;
    model.removeAllTracks([&](std::string error) {
        clearError = std::move(error);
    });
    pumpUntil([&] { return clearError.has_value(); });
    ASSERT_TRUE(clearError.has_value());
    ASSERT_TRUE(clearError->empty()) << *clearError;
    EXPECT_EQ(model.trackCount(), 0u);
    result = moveHistory(model, false);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->empty()) << *result;
    EXPECT_EQ(model.trackCount(), initialTrackCount + 2);
    result = moveHistory(model, true);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->empty()) << *result;
    EXPECT_EQ(model.trackCount(), 0u);
}

TEST(AppLayerUndoIntegrationTest, EmptyMidiClipCreationCompoundUndoAndRedo) {
    ScopedTestEventLoop eventLoop;
    auto* dispatcher = uapmd::defaultDeviceIODispatcher();
    uapmd_app::AppModel model(256, 65536, 48000, dispatcher);

    std::optional<int32_t> trackIndex;
    model.addTrack([&](int32_t index, std::string) {
        trackIndex = index;
    });
    pumpUntil([&] { return trackIndex.has_value(); });
    ASSERT_TRUE(trackIndex.has_value());
    ASSERT_GE(*trackIndex, 0);

    const auto clip = model.createEmptyMidiClip(*trackIndex);
    ASSERT_TRUE(clip.success) << clip.error;
    auto* track = model.getTimelineTracks()[static_cast<size_t>(*trackIndex)];
    ASSERT_EQ(track->clipManager().clipCount(), 1u);

    auto result = moveHistory(model, false);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->empty()) << *result;
    EXPECT_EQ(track->clipManager().clipCount(), 0u);
    result = moveHistory(model, true);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->empty()) << *result;
    EXPECT_EQ(track->clipManager().clipCount(), 1u);
}

TEST(AppLayerUndoIntegrationTest, JavaScriptTrackJobParticipatesInHistory) {
    ScopedTestEventLoop eventLoop;
    struct ScopedAppModelInstance {
        ScopedAppModelInstance() { uapmd_app::AppModel::instantiate(); }
        ~ScopedAppModelInstance() { uapmd_app::AppModel::cleanupInstance(); }
    } appModelInstance;

    auto& model = uapmd_app::AppModel::instance();
    const auto initialTrackCount = model.trackCount();
    uapmd_app::UapmdJSRuntime runtime;
    runtime.ensureApiBootstrapped();
    auto job = choc::json::parse(
        runtime.evaluateScript("uapmd.sequencer.addTrack()"));
    ASSERT_TRUE(job.isObject());
    const auto jobId = job["jobId"].get<int32_t>();
    ASSERT_GT(jobId, 0);

    for (int i = 0; i < 200 && job["state"].get<std::string>() == "running"; ++i) {
        remidy::EventLoop::processQueuedTasks();
        job = choc::json::parse(runtime.evaluateScript(
            "uapmd.mutations.getJob(" + std::to_string(jobId) + ")"));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    ASSERT_EQ(job["state"].get<std::string>(), "completed")
        << job["error"].get<std::string>();
    EXPECT_EQ(
        job["result"]["trackIndex"].get<int32_t>(),
        static_cast<int32_t>(initialTrackCount));
    ASSERT_EQ(model.trackCount(), initialTrackCount + 1);

    auto result = moveHistory(model, false);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->empty()) << *result;
    EXPECT_EQ(model.trackCount(), initialTrackCount);
}

TEST(AppLayerUndoIntegrationTest, JavaScriptUndoAndRedoUseMutationJobs) {
    ScopedTestEventLoop eventLoop;
    struct ScopedAppModelInstance {
        ScopedAppModelInstance() { uapmd_app::AppModel::instantiate(); }
        ~ScopedAppModelInstance() { uapmd_app::AppModel::cleanupInstance(); }
    } appModelInstance;

    auto& model = uapmd_app::AppModel::instance();
    const auto initialTrackCount = model.trackCount();
    uapmd_app::UapmdJSRuntime runtime;
    runtime.ensureApiBootstrapped();

    auto addJob = choc::json::parse(runtime.evaluateScript("uapmd.sequencer.addTrack()"));
    ASSERT_TRUE(addJob.isObject());
    const auto addJobId = addJob["jobId"].get<int32_t>();
    ASSERT_GT(addJobId, 0);
    for (int i = 0; i < 200 && addJob["state"].get<std::string>() == "running"; ++i) {
        remidy::EventLoop::processQueuedTasks();
        addJob = choc::json::parse(runtime.evaluateScript(
            "uapmd.mutations.getJob(" + std::to_string(addJobId) + ")"));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_EQ(addJob["state"].get<std::string>(), "completed")
        << addJob["error"].get<std::string>();
    ASSERT_EQ(model.trackCount(), initialTrackCount + 1);

    auto history = choc::json::parse(runtime.evaluateScript(
        "uapmd.sequencer.getHistoryState()"));
    EXPECT_TRUE(history["canUndo"].get<bool>());
    EXPECT_FALSE(history["canRedo"].get<bool>());

    auto pollHistoryJob = [&runtime](const char* operation) {
        auto job = choc::json::parse(runtime.evaluateScript(operation));
        EXPECT_TRUE(job.isObject());
        const auto jobId = job["jobId"].get<int32_t>();
        EXPECT_GT(jobId, 0);
        for (int i = 0; i < 200 && job["state"].get<std::string>() == "running"; ++i) {
            remidy::EventLoop::processQueuedTasks();
            job = choc::json::parse(runtime.evaluateScript(
                "uapmd.mutations.getJob(" + std::to_string(jobId) + ")"));
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return job;
    };

    auto undoJob = pollHistoryJob("uapmd.sequencer.undo()");
    ASSERT_EQ(undoJob["state"].get<std::string>(), "completed")
        << undoJob["error"].get<std::string>();
    EXPECT_TRUE(undoJob["result"]["success"].get<bool>());
    EXPECT_EQ(model.trackCount(), initialTrackCount);

    auto redoJob = pollHistoryJob("uapmd.sequencer.redo()");
    ASSERT_EQ(redoJob["state"].get<std::string>(), "completed")
        << redoJob["error"].get<std::string>();
    EXPECT_TRUE(redoJob["result"]["success"].get<bool>());
    EXPECT_EQ(model.trackCount(), initialTrackCount + 1);
}

TEST(AppLayerUndoIntegrationTest, JavaScriptNamedCompoundScopeUsesMutationJobs) {
    ScopedTestEventLoop eventLoop;
    struct ScopedAppModelInstance {
        ScopedAppModelInstance() { uapmd_app::AppModel::instantiate(); }
        ~ScopedAppModelInstance() { uapmd_app::AppModel::cleanupInstance(); }
    } appModelInstance;

    auto& model = uapmd_app::AppModel::instance();
    const auto initialTrackCount = model.trackCount();
    uapmd_app::UapmdJSRuntime runtime;
    runtime.ensureApiBootstrapped();

    auto scope = choc::json::parse(runtime.evaluateScript(
        "uapmd.sequencer.beginCompound('Remote batch')"));
    ASSERT_TRUE(scope["success"].get<bool>()) << scope["error"].get<std::string>();

    auto pollJob = [&runtime](const char* expression) {
        auto job = choc::json::parse(runtime.evaluateScript(expression));
        EXPECT_TRUE(job.isObject());
        if (!job.isObject())
            return job;
        const auto jobId = job["jobId"].get<int32_t>();
        EXPECT_GT(jobId, 0);
        if (jobId <= 0)
            return job;
        for (int i = 0; i < 200 && job["state"].get<std::string>() == "running"; ++i) {
            remidy::EventLoop::processQueuedTasks();
            job = choc::json::parse(runtime.evaluateScript(
                "uapmd.mutations.getJob(" + std::to_string(jobId) + ")"));
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return job;
    };

    auto addJob = pollJob("uapmd.sequencer.addTrack()");
    ASSERT_EQ(addJob["state"].get<std::string>(), "completed")
        << addJob["error"].get<std::string>();
    ASSERT_EQ(model.trackCount(), initialTrackCount + 1);

    auto endJob = pollJob("uapmd.sequencer.endCompound()");
    ASSERT_EQ(endJob["state"].get<std::string>(), "completed")
        << endJob["error"].get<std::string>();
    ASSERT_TRUE(endJob["result"]["success"].get<bool>());

    auto undoJob = pollJob("uapmd.sequencer.undo()");
    ASSERT_EQ(undoJob["state"].get<std::string>(), "completed")
        << undoJob["error"].get<std::string>();
    ASSERT_TRUE(undoJob["result"]["success"].get<bool>());
    EXPECT_EQ(model.trackCount(), initialTrackCount);
}
