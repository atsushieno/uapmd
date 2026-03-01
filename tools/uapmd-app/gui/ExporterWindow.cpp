#include "ExporterWindow.hpp"

#include <cstring>

#include "PlatformDialogs.hpp"
#include "../AppModel.hpp"

namespace uapmd::gui {

ExporterWindow::ExporterWindow(Callbacks callbacks)
    : callbacks_(std::move(callbacks)) {}

void ExporterWindow::setCallbacks(Callbacks callbacks) {
    callbacks_ = std::move(callbacks);
}

void ExporterWindow::open() {
    visible_ = true;
    refreshBounds();
}

void ExporterWindow::hide() {
    visible_ = false;
}

void ExporterWindow::render(float uiScale) {
    if (!visible_)
        return;

    const std::string windowId = "RenderToFile";
    if (callbacks_.setChildSize)
        callbacks_.setChildSize(windowId, ImVec2(560.0f, 560.0f));

    if (!ImGui::Begin("Render To File", &visible_)) {
        ImGui::End();
        return;
    }

    if (callbacks_.updateChildSizeState)
        callbacks_.updateChildSizeState(windowId);

    if (ImGui::IsWindowAppearing())
        refreshBounds();

    auto& app = uapmd::AppModel::instance();
    auto bounds = app.timelineContentBounds();
    auto status = app.getRenderToFileStatus();

    ImGui::TextWrapped("Render the current timeline to a WAV file using offline processing.");
    ImGui::Separator();

    ImGui::TextUnformatted("Output");
    ImGui::InputText("##RenderOutputPath", state_.outputPath.data(), state_.outputPath.size());
    ImGui::SameLine();
    if (ImGui::Button("Browse...")) {
        std::vector<uapmd::DocumentFilter> filters{
            {"WAV", {}, {"*.wav"}},
            {"All Files", {}, {"*"}}
        };

        if (auto* provider = uapmd::AppModel::instance().documentProvider()) {
            provider->pickSaveDocument(
                "render.wav",
                filters,
                [this](uapmd::DocumentPickResult result) {
                    if (!result.success || result.handles.empty())
                        return;
                    auto pathStr = result.handles[0].id;
                    std::strncpy(state_.outputPath.data(), pathStr.c_str(), state_.outputPath.size() - 1);
                    state_.outputPath[state_.outputPath.size() - 1] = '\0';
                    lastRenderPath_ = std::filesystem::path(pathStr);
                }
            );
        }
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Range");
    bool entire = state_.rangeMode == RenderDialogState::RangeMode::EntireProject;
    bool loop = state_.rangeMode == RenderDialogState::RangeMode::LoopRegion;
    bool custom = state_.rangeMode == RenderDialogState::RangeMode::Custom;

    if (ImGui::RadioButton("Entire Project", entire))
        state_.rangeMode = RenderDialogState::RangeMode::EntireProject;

    if (state_.hasLoopRange) {
        if (ImGui::RadioButton("Loop Region", loop))
            state_.rangeMode = RenderDialogState::RangeMode::LoopRegion;
        ImGui::SameLine();
        ImGui::TextDisabled("(%.2f s → %.2f s)", state_.loopStartSeconds, state_.loopEndSeconds);
    } else {
        ImGui::BeginDisabled();
        bool disabledLoop = false;
        ImGui::RadioButton("Loop Region", disabledLoop);
        ImGui::EndDisabled();
    }

    if (ImGui::RadioButton("Custom", custom))
        state_.rangeMode = RenderDialogState::RangeMode::Custom;

    if (state_.rangeMode == RenderDialogState::RangeMode::Custom) {
        ImGui::InputDouble("Start (s)", &state_.customStartSeconds, 0.1, 1.0, "%.3f");
        ImGui::InputDouble("End (s)", &state_.customEndSeconds, 0.1, 1.0, "%.3f");
        if (state_.customEndSeconds < state_.customStartSeconds)
            state_.customEndSeconds = state_.customStartSeconds;
    } else {
        ImGui::TextDisabled("Use custom mode to edit absolute start/end times.");
    }

    if (bounds.hasContent && state_.rangeMode == RenderDialogState::RangeMode::EntireProject) {
        ImGui::TextDisabled("Detected project length: %.2f s", bounds.durationSeconds);
    } else if (!bounds.hasContent && state_.rangeMode == RenderDialogState::RangeMode::EntireProject) {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 180, 120, 255));
        ImGui::TextWrapped("No clips detected. Choose a custom range or enable loop playback markers.");
        ImGui::PopStyleColor();
    }

    ImGui::Separator();
    ImGui::InputDouble("Tail / Guard (s)", &state_.guardSeconds, 0.1, 1.0, "%.2f");
    if (state_.guardSeconds < 0.0)
        state_.guardSeconds = 0.0;
    ImGui::Checkbox("Stop after silence", &state_.silenceStopEnabled);
    if (state_.silenceStopEnabled) {
        ImGui::InputDouble("Hold Length (s)", &state_.silenceHoldSeconds, 0.5, 1.0, "%.1f");
        if (state_.silenceHoldSeconds < 0.0)
            state_.silenceHoldSeconds = 0.0;
        ImGui::InputDouble("Threshold (dB)", &state_.silenceThresholdDb, 1.0, 6.0, "%.0f");
    }

    ImGui::Separator();
    if (!status.message.empty())
        ImGui::TextWrapped("%s", status.message.c_str());
    ImGui::ProgressBar(static_cast<float>(status.progress), ImVec2(-FLT_MIN, 0.0f));

    bool canStart = !status.running && state_.outputPath[0] != '\0';
    if (state_.rangeMode == RenderDialogState::RangeMode::EntireProject && !bounds.hasContent)
        canStart = false;
    if (state_.rangeMode == RenderDialogState::RangeMode::LoopRegion && !state_.hasLoopRange)
        canStart = false;

    if (!canStart)
        ImGui::BeginDisabled();
    if (ImGui::Button("Start Render")) {
        uapmd::AppModel::RenderToFileSettings settings{};
        settings.outputPath = std::filesystem::path(state_.outputPath.data());
        settings.tailSeconds = state_.guardSeconds;
        settings.enableSilenceStop = state_.silenceStopEnabled;
        settings.silenceDurationSeconds = state_.silenceHoldSeconds;
        settings.silenceThresholdDb = state_.silenceThresholdDb;
        settings.contentBoundsValid = bounds.hasContent;
        settings.contentStartSeconds = bounds.startSeconds;
        settings.contentEndSeconds = bounds.endSeconds;
        settings.useContentFallback = (state_.rangeMode == RenderDialogState::RangeMode::EntireProject);

        switch (state_.rangeMode) {
            case RenderDialogState::RangeMode::EntireProject:
                settings.startSeconds = bounds.hasContent ? bounds.startSeconds : 0.0;
                settings.endSeconds.reset();
                break;
            case RenderDialogState::RangeMode::LoopRegion:
                settings.startSeconds = state_.loopStartSeconds;
                settings.endSeconds = state_.loopEndSeconds;
                break;
            case RenderDialogState::RangeMode::Custom:
                settings.startSeconds = std::max(0.0, state_.customStartSeconds);
                settings.endSeconds = std::max(settings.startSeconds, state_.customEndSeconds);
                break;
        }

        if (!app.startRenderToFile(settings)) {
            platformError("Render To File", "Unable to start render job.");
        } else {
            lastRenderPath_ = settings.outputPath;
        }
    }
    if (!canStart)
        ImGui::EndDisabled();

    ImGui::SameLine();
    if (status.running) {
        if (ImGui::Button("Cancel")) {
            app.cancelRenderToFile();
        }
    } else if (status.completed) {
        if (ImGui::Button("Dismiss")) {
            app.clearCompletedRenderStatus();
        }
    }

    ImGui::End();
}

void ExporterWindow::refreshBounds() {
    ensureDefaultPath();

    auto& app = uapmd::AppModel::instance();
    auto bounds = app.timelineContentBounds();
    if (bounds.hasContent) {
        state_.customStartSeconds = bounds.startSeconds;
        state_.customEndSeconds = bounds.endSeconds;
    }

    auto& timeline = app.timeline();
    if (timeline.loopEnabled && timeline.loopEnd.samples > timeline.loopStart.samples) {
        state_.hasLoopRange = true;
        state_.loopStartSeconds = timeline.loopStart.toSeconds(app.sampleRate());
        state_.loopEndSeconds = timeline.loopEnd.toSeconds(app.sampleRate());
    } else {
        state_.hasLoopRange = false;
    }
}

void ExporterWindow::ensureDefaultPath() {
    if (state_.outputPath[0] != '\0')
        return;

    std::filesystem::path defaultPath = lastRenderPath_;
    if (defaultPath.empty())
        defaultPath = std::filesystem::current_path() / "render.wav";

    auto pathStr = defaultPath.string();
    std::strncpy(state_.outputPath.data(), pathStr.c_str(), state_.outputPath.size() - 1);
    state_.outputPath[state_.outputPath.size() - 1] = '\0';
}

} // namespace uapmd::gui
