#include "MidiDumpWindow.hpp"

#include <format>

namespace uapmd::gui {

namespace {
constexpr float kDefaultWindowWidth = 620.0f;
constexpr float kDefaultWindowHeight = 420.0f;
}

void MidiDumpWindow::showClipDump(ClipDumpData data) {
    if (data.trackIndex < 0 || data.clipId < 0) {
        return;
    }

    const auto key = std::make_pair(data.trackIndex, data.clipId);
    auto& state = windows_[key];
    state.data = std::move(data);
    state.visible = true;
}

void MidiDumpWindow::closeClip(int32_t trackIndex, int32_t clipId) {
    windows_.erase(std::make_pair(trackIndex, clipId));
}

void MidiDumpWindow::render(const RenderContext& context) {
    std::vector<std::pair<int32_t, int32_t>> keys;
    keys.reserve(windows_.size());
    for (const auto& entry : windows_) {
        keys.push_back(entry.first);
    }

    for (const auto& key : keys) {
        auto it = windows_.find(key);
        if (it == windows_.end()) {
            continue;
        }
        if (!it->second.visible) {
            continue;
        }
        renderWindow(it->second, context);
    }
}

std::string MidiDumpWindow::buildWindowTitle(const ClipDumpData& data) {
    return std::format(
        "MIDI Dump - Track {} Clip {}###MidiDump{}_{}",
        data.trackIndex + 1,
        data.clipId,
        data.trackIndex,
        data.clipId
    );
}

void MidiDumpWindow::renderWindow(WindowState& state, const RenderContext& context) {
    auto& dump = state.data;
    std::string windowTitle = buildWindowTitle(dump);
    bool windowOpen = state.visible;
    const std::string windowSizeId = std::format("MidiDumpWindow{}_{}", dump.trackIndex, dump.clipId);

    if (context.setNextChildWindowSize) {
        context.setNextChildWindowSize(
            windowSizeId,
            ImVec2(kDefaultWindowWidth * context.uiScale, kDefaultWindowHeight * context.uiScale)
        );
    }

    if (ImGui::Begin(windowTitle.c_str(), &windowOpen, ImGuiWindowFlags_NoCollapse)) {
        if (context.updateChildWindowSizeState) {
            context.updateChildWindowSizeState(windowSizeId);
        }

        ImGui::TextUnformatted(dump.clipName.c_str());
        if (!dump.fileLabel.empty()) {
            ImGui::Text("File: %s", dump.fileLabel.c_str());
        }
        ImGui::Text("Tempo: %.2f BPM | PPQ: %u", dump.tempo, dump.tickResolution);

        if (context.reloadClip) {
            if (ImGui::Button("Refresh")) {
                auto refreshed = context.reloadClip(dump.trackIndex, dump.clipId);
                if (refreshed.trackIndex == dump.trackIndex && refreshed.clipId == dump.clipId) {
                    state.data = std::move(refreshed);
                }
            }
        }

        ImGui::Separator();

        if (!dump.success) {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "Failed to load clip: %s", dump.error.c_str());
        } else if (dump.events.empty()) {
            ImGui::TextUnformatted("No MIDI events available in this clip.");
        } else {
            const ImGuiTableFlags tableFlags =
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                ImGuiTableFlags_ScrollY | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable;

            if (ImGui::BeginTable("MidiDumpTable", 3, tableFlags, ImVec2(0, 0))) {
                ImGui::TableSetupColumn("Time (s)", ImGuiTableColumnFlags_WidthFixed, 120.0f * context.uiScale);
                ImGui::TableSetupColumn("Length", ImGuiTableColumnFlags_WidthFixed, 80.0f * context.uiScale);
                ImGui::TableSetupColumn("Message Bytes", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();

                ImGuiListClipper clipper;
                clipper.Begin(static_cast<int>(dump.events.size()));
                while (clipper.Step()) {
                    for (int rowIndex = clipper.DisplayStart; rowIndex < clipper.DisplayEnd; ++rowIndex) {
                        const auto& row = dump.events[static_cast<size_t>(rowIndex)];
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextUnformatted(row.timeLabel.c_str());
                        ImGui::TableSetColumnIndex(1);
                        ImGui::Text("%zu", row.lengthBytes);
                        ImGui::TableSetColumnIndex(2);
                        ImGui::TextUnformatted(row.hexBytes.c_str());
                    }
                }

                ImGui::EndTable();
            }
        }
    }
    ImGui::End();
    state.visible = windowOpen;
}

} // namespace uapmd::gui
