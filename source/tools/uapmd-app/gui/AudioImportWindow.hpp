#pragma once

#include <array>
#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

#include <imgui.h>
#include <uapmd-data/uapmd-data.hpp>

namespace uapmd::gui {

class AudioImportWindow {
public:
    struct Callbacks {
        std::function<void(const std::string&, ImVec2)> setChildSize;
        std::function<void(const std::string&)> updateChildSizeState;
        std::function<void(uapmd::import::AudioImportResult&&)> applyImportResult;
    };

    AudioImportWindow() = default;
    explicit AudioImportWindow(Callbacks callbacks);

    void setCallbacks(Callbacks callbacks);

    void open();
    void hide();
    bool isVisible() const { return visible_; }

    void setAudioFile(const std::string& path);
    void setModelFile(const std::string& path);

    void render(float uiScale);

private:
    struct ImportDialogState {
        std::array<char, 1024> audioPath{};
        std::array<char, 1024> modelPath{};
    };

    struct ImportJobStatus {
        bool running{false};
        bool completed{false};
        bool success{false};
        bool canceled{false};
        float progress{0.0f};
        std::string message;
        std::string error;
    };

    struct ImportJobState {
        std::atomic<bool> cancel{false};
    };

    Callbacks callbacks_{};
    bool visible_{false};
    ImportDialogState state_{};

    mutable std::mutex statusMutex_;
    ImportJobStatus jobStatus_{};

    std::optional<uapmd::import::AudioImportResult> pendingResult_;
    std::mutex resultMutex_;

    std::shared_ptr<ImportJobState> activeJob_;
    mutable std::mutex jobMutex_;

    bool hasPathsReady() const;
    void browseForAudioFile();
    void browseForModelFile();
    void startImportJob();
    void requestCancelActiveJob();
    void runImportJob(std::shared_ptr<ImportJobState> job,
                      std::string audioPath,
                      std::string modelPath);
    void updateStatus(const std::function<void(ImportJobStatus&)>& updater);
    ImportJobStatus snapshotStatus() const;
    void resetStatus();
    void processCompletedResult(const ImportJobStatus& statusSnapshot);
};

} // namespace uapmd::gui
