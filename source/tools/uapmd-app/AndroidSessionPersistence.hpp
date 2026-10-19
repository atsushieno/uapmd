#pragma once

#include <uapmd-app-model/uapmd-app-model.hpp>
#include <uapmd-data/uapmd-data.hpp>
#include <uapmd-plugin-hosting/uapmd-plugin-hosting.hpp>
#include <remidy/remidy.hpp>
#include <SDL3/SDL.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <functional>
#include <memory>
#include <thread>

namespace uapmd_app {

// Owned by the Android application loop, on the model thread. SDL emits its
// background event while pumping that thread, before suspending it; waiting
// until the event is polled can be too late to persist anything.
class AndroidSessionPersistence {
    struct StagingDirectory {
        std::filesystem::path path;
        ~StagingDirectory() {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    };

    std::filesystem::path snapshot_path_;
    std::shared_future<AppModel::ProjectResult> pending_save_;
    std::shared_ptr<bool> restore_complete_{std::make_shared<bool>(false)};
    std::shared_ptr<bool> restore_failed_{std::make_shared<bool>(false)};
    bool watching_{false};

    static bool SDLCALL watchLifecycle(void* context, SDL_Event* event) {
        if (event->type == SDL_EVENT_WILL_ENTER_BACKGROUND) {
            // Android's lifecycle events run on the SDL/model thread. Never
            // serialize the model from another thread (e.g. a pushed event).
            if (remidy::EventLoop::runningOnMainThread())
                static_cast<AndroidSessionPersistence*>(context)->save();
        }
        return true;
    }

public:
    AndroidSessionPersistence() {
        const auto directory = uapmd_plugin_hosting::applicationDataDirectory();
        if (!directory.empty())
            snapshot_path_ = directory / "android-session.uapmdz";
        // A successful explicit New/Open also allows recovery after an earlier
        // restore failed; keep that failed snapshot intact until then.
        AppModel::instance().projectLoaded.push_back([failed = restore_failed_] {
            *failed = false;
        });
        watching_ = SDL_AddEventWatch(watchLifecycle, this);
        if (!watching_)
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Cannot watch Android lifecycle: %s", SDL_GetError());
    }

    ~AndroidSessionPersistence() { stop(); }

    void stop() {
        if (watching_)
            SDL_RemoveEventWatch(watchLifecycle, this);
        watching_ = false;
    }

    void restore(std::function<void()> completion) {
        std::error_code ec;
        if (snapshot_path_.empty() || !std::filesystem::exists(snapshot_path_, ec)) {
            *restore_complete_ = true;
            completion();
            return;
        }
        AppModel::instance().loadProjectFromResolvedPath(snapshot_path_,
            [ready = restore_complete_, failed = restore_failed_,
             completion = std::move(completion)](AppModel::ProjectResult result) {
                *ready = true;
                *failed = !result.success;
                if (!result.success)
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Session restore failed: %s", result.error.c_str());
                else {
                    // Recovery is not an explicit user save. Keep the recovered
                    // document dirty so closing/replacing it still prompts.
                    AppModel::instance().sequencer().engine()->timeline().commands().history().clear(false);
                    SDL_Log("Restored Android working session");
                }
                completion();
            });
    }

    void save() {
        if (snapshot_path_.empty() || !*restore_complete_ || *restore_failed_)
            return;
        if (!pending_save_.valid() || pending_save_.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            beginSave();

        // Plug-in state callbacks post work to the model thread. Drain that
        // queue before SDL suspends it, without waiting indefinitely for a
        // misbehaving plug-in. A late save can finish on resume.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (pending_save_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            remidy::EventLoop::processQueuedTasks();
            if (std::chrono::steady_clock::now() >= deadline) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Session save still waiting for plug-in state");
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        const auto result = pending_save_.get();
        if (!result.success)
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Session save failed: %s", result.error.c_str());
    }

private:
    void beginSave() {
        auto promise = std::make_shared<std::promise<AppModel::ProjectResult>>();
        pending_save_ = promise->get_future().share();
        auto stage = std::make_shared<StagingDirectory>();
        const auto generation = std::chrono::steady_clock::now().time_since_epoch().count();
        stage->path = snapshot_path_.parent_path()
            / ("android-session-staging-" + std::to_string(generation));

        auto& model = AppModel::instance();
        auto* engine = model.sequencer().engine();
        // Recorded MIDI is buffered until recording stops. Commit it before
        // building the snapshot, so the current take is included as well.
        engine->notifyRecordingStopped();
        if (model.transport().isRecording())
            model.transport().record();
        uapmd::TimelineFacade::ProjectSaveOptions options;
        for (size_t index = 0; index < engine->tracks().size(); ++index)
            if (model.isTrackHidden(static_cast<int32_t>(index)))
                options.excludedTrackIndexes.push_back(static_cast<int32_t>(index));
        options.emitDocumentEvent = false;
        options.markHistorySaved = false;
        options.copy_media_files = true;
        options.preserve_empty_tracks = true;
        engine->timeline().saveProject(stage->path / "project.uapmd", std::move(options),
            [promise, stage, snapshot = snapshot_path_](uapmd::TimelineFacade::ProjectResult result) {
                if (!result.success) {
                    promise->set_value({false, std::move(result.error)});
                    return;
                }
                try {
                    std::vector<uint8_t> archive;
                    std::string error;
                    if (!uapmd::ProjectArchive::createArchive(stage->path, archive, error)) {
                        promise->set_value({false, std::move(error)});
                        return;
                    }
                    auto temporary = snapshot;
                    temporary += ".tmp";
                    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
                    output.write(reinterpret_cast<const char*>(archive.data()), static_cast<std::streamsize>(archive.size()));
                    output.close();
                    if (!output) {
                        promise->set_value({false, "Cannot write Android session snapshot"});
                        return;
                    }
                    // Same-filesystem rename commits the complete archive in
                    // one step, preserving the previous snapshot on failure.
                    std::filesystem::rename(temporary, snapshot);
                    SDL_Log("Saved Android working session");
                    promise->set_value({true, {}});
                } catch (const std::exception& error) {
                    promise->set_value({false, error.what()});
                }
            });
    }
};

} // namespace uapmd_app
