#include "AudioImportWindow.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <format>
#include <utility>
#include <thread>
#include <vector>

#include <uapmd-app-model/uapmd-app-model.hpp>
#include "PlatformDialogs.hpp"

using namespace uapmd;

namespace uapmd_app_gui {

namespace {

template <size_t N>
void copyPathToBuffer(std::array<char, N>& buffer, const std::string& path) {
    std::strncpy(buffer.data(), path.c_str(), buffer.size() - 1);
    buffer[buffer.size() - 1] = '\0';
}

} // namespace

AudioImportWindow::AudioImportWindow(Callbacks callbacks)
    : callbacks_(std::move(callbacks)) {}

void AudioImportWindow::setCallbacks(Callbacks callbacks) {
    callbacks_ = std::move(callbacks);
}

void AudioImportWindow::setStemSeparatorRegistry(uapmd::import::StemSeparatorRegistry* registry) {
    separatorRegistry_ = registry;
}

void AudioImportWindow::open() {
    visible_ = true;
    resetStatus();
}

void AudioImportWindow::hide() {
    visible_ = false;
    requestCancelActiveJob();
}

void AudioImportWindow::setAudioFile(const std::string& path) {
    copyPathToBuffer(state_.audioPath, path);
}

void AudioImportWindow::setModelFile(const std::string& path) {
    copyPathToBuffer(state_.modelPath, path);
}

void AudioImportWindow::render(float /*uiScale*/) {
    if (!visible_)
        return;

    const std::string windowId = "AudioImport";
    if (callbacks_.setChildSize)
        callbacks_.setChildSize(windowId, ImVec2(520.0f, 360.0f));

    bool keepVisible = visible_;
    if (!ImGui::Begin("Split Audio Import", &keepVisible)) {
        ImGui::End();
        visible_ = keepVisible;
        return;
    }
    visible_ = keepVisible;

    if (callbacks_.updateChildSizeState)
        callbacks_.updateChildSizeState(windowId);

    auto statusSnapshot = snapshotStatus();

    // Stem separation is contributed by addins; without one there is nothing
    // this window can do, so say so rather than offering a dead Start button.
    const auto separators = separatorRegistry_
        ? separatorRegistry_->separators()
        : std::vector<uapmd::import::StemSeparator*>{};
    if (separators.empty()) {
        ImGui::TextWrapped(
            "No stem separation addin is enabled. Enable one (such as Demucs) in the Addin Manager.");
        if (ImGui::Button("Close"))
            hide();
        ImGui::End();
        // A run that finished just before its addin was disabled still has a
        // result to hand over.
        processCompletedResult(statusSnapshot);
        return;
    }

    auto* separator = separators.front();
    for (auto* candidate : separators) {
        if (candidate && candidate->id() == selectedSeparatorId_) {
            separator = candidate;
            break;
        }
    }
    selectedSeparatorId_ = std::string(separator->id());
    const auto modelSpec = separator->modelFileSpec();

    ImGui::TextWrapped("Separate a single audio file into stems using %s.",
                       std::string(separator->name()).c_str());
    ImGui::Separator();

    if (separators.size() > 1) {
        if (statusSnapshot.running)
            ImGui::BeginDisabled();
        ImGui::TextUnformatted("Separator");
        if (ImGui::BeginCombo("##SplitSeparator", std::string(separator->name()).c_str())) {
            for (auto* candidate : separators) {
                if (!candidate)
                    continue;
                const bool selected = candidate == separator;
                if (ImGui::Selectable(std::string(candidate->name()).c_str(), selected))
                    selectedSeparatorId_ = std::string(candidate->id());
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        if (statusSnapshot.running)
            ImGui::EndDisabled();
    }

    ImGui::TextUnformatted("Audio File");
    ImGui::InputText("##SplitAudioPath", state_.audioPath.data(), state_.audioPath.size());
    ImGui::SameLine();
    if (ImGui::Button("Browse Audio...")) {
        browseForAudioFile();
    }

    if (modelSpec.required()) {
        ImGui::TextUnformatted(modelSpec.label.c_str());
        ImGui::InputText("##SplitModelPath", state_.modelPath.data(), state_.modelPath.size());
        ImGui::SameLine();
        if (ImGui::Button("Browse Model...")) {
            browseForModelFile(modelSpec);
        }
    }

    ImGui::Separator();
    if (!statusSnapshot.message.empty())
        ImGui::TextWrapped("%s", statusSnapshot.message.c_str());
    ImGui::ProgressBar(statusSnapshot.progress, ImVec2(-FLT_MIN, 0.0f));

    bool canStart = !statusSnapshot.running && hasPathsReady(modelSpec);
    if (!canStart)
        ImGui::BeginDisabled();
    if (ImGui::Button("Start Import")) {
        startImportJob(*separator);
    }
    if (!canStart)
        ImGui::EndDisabled();

    ImGui::SameLine();
    if (statusSnapshot.running) {
        if (ImGui::Button("Cancel")) {
            requestCancelActiveJob();
        }
    } else {
        if (ImGui::Button("Close")) {
            hide();
        }
    }

    ImGui::End();

    if (!visible_ && statusSnapshot.running)
        requestCancelActiveJob();

    processCompletedResult(statusSnapshot);
}

bool AudioImportWindow::hasPathsReady(const uapmd::import::StemSeparatorModelFileSpec& modelSpec) const {
    if (state_.audioPath[0] == '\0')
        return false;
    return !modelSpec.required() || state_.modelPath[0] != '\0';
}

void AudioImportWindow::browseForAudioFile() {
    std::vector<uapmd::DocumentFilter> filters{
        {"Audio Files", {}, {"*.wav", "*.flac", "*.ogg"}},
        {"All Files",   {}, {"*"}}
    };

    if (auto* provider = uapmd_app::AppModel::instance().documentProvider()) {
        provider->pickOpenDocuments(
            filters,
            false,
            [this](uapmd::DocumentPickResult result) {
                if (!result.success || result.handles.empty())
                    return;
                resolveDocumentHandle(
                    result.handles[0],
                    [this](const std::filesystem::path& resolved) {
                        setAudioFile(resolved.string());
                    },
                    [](const std::string& error) {
                        platformError("Split Audio Import", error);
                    });
            }
        );
    } else {
        platformError("Split Audio Import", "Document provider unavailable. Cannot select files.");
    }
}

void AudioImportWindow::browseForModelFile(const uapmd::import::StemSeparatorModelFileSpec& modelSpec) {
    std::vector<uapmd::DocumentFilter> filters{
        {modelSpec.label, {}, modelSpec.extensions},
        {"All Files", {}, {"*"}}
    };

    if (auto* provider = uapmd_app::AppModel::instance().documentProvider()) {
        provider->pickOpenDocuments(
            filters,
            false,
            [this](uapmd::DocumentPickResult result) {
                if (!result.success || result.handles.empty())
                    return;
                resolveDocumentHandle(
                    result.handles[0],
                    [this](const std::filesystem::path& resolved) {
                        setModelFile(resolved.string());
                    },
                    [](const std::string& error) {
                        platformError("Split Audio Import", error);
                    });
            }
        );
    } else {
        platformError("Split Audio Import", "Document provider unavailable. Cannot select files.");
    }
}

void AudioImportWindow::startImportJob(const uapmd::import::StemSeparator& separator) {
    const auto modelSpec = separator.modelFileSpec();
    std::string separatorId{separator.id()};
    std::string audioPath = state_.audioPath.data();
    std::string modelPath = modelSpec.required() ? std::string(state_.modelPath.data()) : std::string{};

    if (audioPath.empty())
        return;
    if (modelSpec.required() && modelPath.empty())
        return;

    if (!std::filesystem::exists(audioPath)) {
        platformError("Split Audio Import", "The selected audio file does not exist.");
        return;
    }

    if (modelSpec.required() && !std::filesystem::exists(modelPath)) {
        platformError("Split Audio Import",
                      std::format("The selected {} does not exist.", modelSpec.label));
        return;
    }

    auto job = std::make_shared<ImportJobState>();
    {
        std::scoped_lock lock(jobMutex_);
        if (activeJob_)
            return;
        activeJob_ = job;
    }

    {
        std::scoped_lock lock(resultMutex_);
        pendingResult_.reset();
    }

    updateStatus([](ImportJobStatus& status) {
        status.running = true;
        status.completed = false;
        status.success = false;
        status.canceled = false;
        status.progress = 0.0f;
        status.message = "Starting separation...";
        status.error.clear();
    });

    std::thread([this, job, separatorId, audioPath, modelPath]() {
        runImportJob(job, separatorId, audioPath, modelPath);
    }).detach();
}

void AudioImportWindow::requestCancelActiveJob() {
    std::shared_ptr<ImportJobState> job;
    {
        std::scoped_lock lock(jobMutex_);
        job = activeJob_;
    }
    if (job)
        job->cancel.store(true, std::memory_order_release);
}

void AudioImportWindow::runImportJob(std::shared_ptr<ImportJobState> job,
                                     std::string separatorId,
                                     std::string audioPath,
                                     std::string modelPath) {
    auto releaseJob = [this, &job]() {
        std::scoped_lock lock(jobMutex_);
        if (activeJob_ == job)
            activeJob_.reset();
    };

    auto progressUpdater = [this](float value, const std::string& message) {
        updateStatus([&](ImportJobStatus& status) {
            status.progress = std::clamp(value, 0.0f, 1.0f);
            status.message = message;
        });
    };

    uapmd::import::AudioImportResult result;

    // The lease keeps the contributing addin's separator alive for the whole
    // run; withdrawing it (by disabling the addin) cancels the run instead of
    // unloading the code underneath it.
    auto lease = separatorRegistry_
        ? separatorRegistry_->acquire(separatorId)
        : uapmd::import::StemSeparatorRegistry::Lease{};
    if (!lease) {
        result.error = "The selected stem separator is no longer available.";
    } else {
        uapmd::import::TrackImporter::AudioImportOptions options;
        options.separator = lease.get();
        options.modelPath = std::move(modelPath);
        options.progressCallback = progressUpdater;
        options.shouldCancel = [job, &lease]() {
            return job->cancel.load(std::memory_order_acquire) || lease.withdrawn();
        };

        result = uapmd::import::TrackImporter::importAudioFile(audioPath, options);
    }
    lease.release();

    releaseJob();

    updateStatus([&](ImportJobStatus& status) {
        status.running = false;
        status.completed = true;
        status.success = result.success;
        status.canceled = result.canceled;
        status.progress = result.success ? 1.0f : status.progress;
        if (result.success) {
            status.message = "Import complete.";
            status.error.clear();
        } else if (result.canceled) {
            status.message = "Import canceled.";
            status.error.clear();
        } else {
            status.message = result.error.empty() ? "Import failed." : result.error;
            status.error = result.error;
        }
    });

    if (result.success) {
        std::scoped_lock lock(resultMutex_);
        pendingResult_ = std::move(result);
    }
}

void AudioImportWindow::updateStatus(const std::function<void(ImportJobStatus&)>& updater) {
    std::scoped_lock lock(statusMutex_);
    updater(jobStatus_);
}

AudioImportWindow::ImportJobStatus AudioImportWindow::snapshotStatus() const {
    std::scoped_lock lock(statusMutex_);
    return jobStatus_;
}

void AudioImportWindow::resetStatus() {
    std::scoped_lock lock(statusMutex_);
    jobStatus_ = {};
    jobStatus_.message.clear();
    jobStatus_.progress = 0.0f;
    {
        std::scoped_lock resultLock(resultMutex_);
        pendingResult_.reset();
    }
}

void AudioImportWindow::processCompletedResult(const ImportJobStatus& statusSnapshot) {
    if (!statusSnapshot.completed || !statusSnapshot.success)
        return;

    std::optional<uapmd::import::AudioImportResult> result;
    {
        std::scoped_lock lock(resultMutex_);
        if (!pendingResult_.has_value())
            return;
        result = std::move(pendingResult_);
        pendingResult_.reset();
    }

    if (result && callbacks_.applyImportResult)
        callbacks_.applyImportResult(std::move(*result));

    resetStatus();
    visible_ = false;
}

} // namespace uapmd_app_gui
