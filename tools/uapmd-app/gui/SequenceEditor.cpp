#include "SequenceEditor.hpp"

#include <algorithm>
#include <format>
#include <iostream>
#include <map>

#include <imgui.h>

#include "../AppModel.hpp"

namespace uapmd::gui {

void SequenceEditor::showWindow(int32_t trackIndex) {
    auto it = windows_.find(trackIndex);
    if (it == windows_.end()) {
        SequenceEditorState state;
        state.visible = true;
        windows_[trackIndex] = state;
    } else {
        it->second.visible = true;
    }
}

void SequenceEditor::hideWindow(int32_t trackIndex) {
    auto it = windows_.find(trackIndex);
    if (it != windows_.end()) {
        it->second.visible = false;
    }
}

bool SequenceEditor::isVisible(int32_t trackIndex) const {
    auto it = windows_.find(trackIndex);
    return it != windows_.end() && it->second.visible;
}

void SequenceEditor::refreshClips(int32_t trackIndex, const std::vector<ClipRow>& clips) {
    auto it = windows_.find(trackIndex);
    if (it != windows_.end()) {
        it->second.displayClips = clips;
    }
}

void SequenceEditor::render(const RenderContext& context) {
    // Collect track indices to avoid iterator invalidation
    std::vector<int32_t> trackIndices;
    trackIndices.reserve(windows_.size());
    for (const auto& entry : windows_) {
        trackIndices.push_back(entry.first);
    }

    for (int32_t trackIndex : trackIndices) {
        auto it = windows_.find(trackIndex);
        if (it == windows_.end()) {
            continue;
        }

        auto& state = it->second;
        if (!state.visible) {
            continue;
        }

        renderWindow(trackIndex, state, context);
    }
}

void SequenceEditor::renderWindow(int32_t trackIndex, SequenceEditorState& state, const RenderContext& context) {
    std::string windowTitle = std::format("Sequence Editor - Track {}###SequenceEditor{}", trackIndex, trackIndex);

    bool windowOpen = state.visible;
    std::string windowSizeId = std::format("SequenceEditorWindow{}", trackIndex);
    float baseWidth = 600.0f;
    const float viewportWidth = ImGui::GetIO().DisplaySize.x;
    if (viewportWidth > 0.0f && context.uiScale > 0.0f) {
        baseWidth = std::min(baseWidth, viewportWidth / context.uiScale);
    }

    if (context.setNextChildWindowSize) {
        context.setNextChildWindowSize(windowSizeId, ImVec2(baseWidth * context.uiScale, 350.0f * context.uiScale));
    }

    ImGui::SetNextWindowSizeConstraints(
        ImVec2(400.0f * context.uiScale, 150.0f * context.uiScale),
        ImVec2(FLT_MAX, FLT_MAX)
    );

    if (ImGui::Begin(windowTitle.c_str(), &windowOpen, ImGuiWindowFlags_None)) {
        if (context.updateChildWindowSizeState) {
            context.updateChildWindowSizeState(windowSizeId);
        }

        // Clips section
        ImGui::Text("Clips:");
        ImGui::Spacing();

        renderClipTable(trackIndex, state, context);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Action buttons
        if (ImGui::Button("New Clip")) {
            if (context.addClip) {
                // addClip callback will open file dialog
                context.addClip(trackIndex, "");  // Empty filepath means show dialog
            }
        }

        ImGui::SameLine();

        if (ImGui::Button("Clear All")) {
            if (!state.displayClips.empty()) {
                ImGui::OpenPopup("Clear All Clips?");
            }
        }

        // Confirmation dialog for Clear All
        if (ImGui::BeginPopupModal("Clear All Clips?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("This will remove all clips from this track.");
            ImGui::Text("Are you sure?");
            ImGui::Spacing();

            if (ImGui::Button("Yes, Clear All", ImVec2(120 * context.uiScale, 0))) {
                if (context.clearAllClips) {
                    context.clearAllClips(trackIndex);
                }
                ImGui::CloseCurrentPopup();
            }

            ImGui::SameLine();

            if (ImGui::Button("Cancel", ImVec2(120 * context.uiScale, 0))) {
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }
    }
    ImGui::End();

    if (!windowOpen) {
        state.visible = false;
    }
}

void SequenceEditor::renderClipTable(int32_t trackIndex, SequenceEditorState& state, const RenderContext& context) {
    // Reserve space for buttons at the bottom (button height + spacing + separator)
    float reservedHeight = (ImGui::GetFrameHeightWithSpacing() * 2.0f);
    float availableHeight = ImGui::GetContentRegionAvail().y - reservedHeight;

    // Render table in a child window with scroll
    if (ImGui::BeginChild("##ClipTableRegion", ImVec2(0, availableHeight), false, ImGuiWindowFlags_None)) {
        ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY;

        if (ImGui::BeginTable("ClipTable", 6, flags)) {
            ImGui::TableSetupColumn("Anchor",  ImGuiTableColumnFlags_WidthFixed, 120.0f * context.uiScale);
            ImGui::TableSetupColumn("Origin",  ImGuiTableColumnFlags_WidthFixed, 100.0f * context.uiScale);
            ImGui::TableSetupColumn("Position",  ImGuiTableColumnFlags_WidthFixed, 70.0f * context.uiScale);
            ImGui::TableSetupColumn("Name",  ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Filename",  ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Delete", ImGuiTableColumnFlags_WidthFixed, 70.0f * context.uiScale);
            ImGui::TableHeadersRow();

            for (const auto& clip : state.displayClips) {
                renderClipRow(trackIndex, clip, context);
            }

            ImGui::EndTable();
        }
    }
    ImGui::EndChild();
}

void SequenceEditor::renderClipRow(int32_t trackIndex, const ClipRow& clip, const RenderContext& context) {
    ImGui::TableNextRow();

    // Anchor column
    ImGui::TableSetColumnIndex(0);

    // Get the anchor label using clip name
    std::string anchorLabel = "Track";
    if (clip.anchorClipId != -1) {
        // Find the anchor clip's name
        auto it = windows_.find(trackIndex);
        if (it != windows_.end()) {
            for (const auto& c : it->second.displayClips) {
                if (c.clipId == clip.anchorClipId) {
                    anchorLabel = c.name.empty() ? std::format("Clip #{}", c.clipId) : c.name;
                    break;
                }
            }
        }
    }

    ImGui::SetNextItemWidth(-FLT_MIN);  // Use all available width in column
    std::string comboId = std::format("##AnchorCombo{}", clip.clipId);
    if (ImGui::BeginCombo(comboId.c_str(), anchorLabel.c_str())) {
        // Track anchor option
        bool isTrackAnchor = (clip.anchorClipId == -1);
        if (ImGui::Selectable("Track", isTrackAnchor)) {
            if (context.updateClip) {
                context.updateClip(trackIndex, clip.clipId, -1, clip.anchorOrigin, clip.position);
            }
        }

        // Other clip anchors - show clip names
        auto anchorOptions = getAnchorOptions(trackIndex, clip.clipId);
        auto it = windows_.find(trackIndex);
        if (it != windows_.end()) {
            for (int32_t anchorId : anchorOptions) {
                bool isSelected = (clip.anchorClipId == anchorId);

                // Find the name of this anchor clip
                std::string anchorName;
                for (const auto& c : it->second.displayClips) {
                    if (c.clipId == anchorId) {
                        anchorName = c.name.empty() ? std::format("Clip #{}", anchorId) : c.name;
                        break;
                    }
                }

                if (ImGui::Selectable(anchorName.c_str(), isSelected)) {
                    if (context.updateClip) {
                        context.updateClip(trackIndex, clip.clipId, anchorId, clip.anchorOrigin, clip.position);
                    }
                }
            }
        }

        ImGui::EndCombo();
    }

    // Origin column
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-FLT_MIN);  // Use all available width in column
    std::string originComboId = std::format("##OriginCombo{}", clip.clipId);
    if (ImGui::BeginCombo(originComboId.c_str(), clip.anchorOrigin.c_str())) {
        bool isStart = (clip.anchorOrigin == "Start");
        if (ImGui::Selectable("Start", isStart)) {
            if (context.updateClip) {
                context.updateClip(trackIndex, clip.clipId, clip.anchorClipId, "Start", clip.position);
            }
        }

        bool isEnd = (clip.anchorOrigin == "End");
        if (ImGui::Selectable("End", isEnd)) {
            if (context.updateClip) {
                context.updateClip(trackIndex, clip.clipId, clip.anchorClipId, "End", clip.position);
            }
        }

        ImGui::EndCombo();
    }

    // Position column
    ImGui::TableSetColumnIndex(2);
    char posBuffer[64];
    strncpy(posBuffer, clip.position.c_str(), sizeof(posBuffer) - 1);
    posBuffer[sizeof(posBuffer) - 1] = '\0';

    ImGui::SetNextItemWidth(-FLT_MIN);  // Use all available width in column
    std::string inputId = std::format("##PosInput{}", clip.clipId);
    if (ImGui::InputText(inputId.c_str(), posBuffer, sizeof(posBuffer), ImGuiInputTextFlags_EnterReturnsTrue)) {
        if (context.updateClip) {
            context.updateClip(trackIndex, clip.clipId, clip.anchorClipId, clip.anchorOrigin, std::string(posBuffer));
        }
    }

    // Name column (editable)
    ImGui::TableSetColumnIndex(3);
    static std::map<int32_t, std::array<char, 256>> nameBuffers;
    if (nameBuffers.find(clip.clipId) == nameBuffers.end()) {
        nameBuffers[clip.clipId] = {};
        strncpy(nameBuffers[clip.clipId].data(), clip.name.c_str(), nameBuffers[clip.clipId].size() - 1);
    }

    ImGui::SetNextItemWidth(-FLT_MIN);  // Use all available width in column
    std::string nameInputId = std::format("##NameInput{}", clip.clipId);
    if (ImGui::InputText(nameInputId.c_str(), nameBuffers[clip.clipId].data(),
                        nameBuffers[clip.clipId].size(), ImGuiInputTextFlags_EnterReturnsTrue)) {
        if (context.updateClipName) {
            context.updateClipName(trackIndex, clip.clipId, std::string(nameBuffers[clip.clipId].data()));
        }
    }

    // Filename column (Change button first, then filename only)
    ImGui::TableSetColumnIndex(4);
    std::string changeButtonId = std::format("Change##{}", clip.clipId);
    if (ImGui::Button(changeButtonId.c_str())) {
        if (context.changeClipFile) {
            context.changeClipFile(trackIndex, clip.clipId);
        }
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(clip.filename.c_str());

    // Delete column
    ImGui::TableSetColumnIndex(5);
    std::string deleteId = std::format("Delete##{}",  clip.clipId);
    if (ImGui::Button(deleteId.c_str())) {
        if (context.removeClip) {
            context.removeClip(trackIndex, clip.clipId);
        }
    }
}

std::vector<int32_t> SequenceEditor::getAnchorOptions(int32_t trackIndex, int32_t currentClipId) const {
    std::vector<int32_t> options;

    auto it = windows_.find(trackIndex);
    if (it == windows_.end()) {
        return options;
    }

    const auto& state = it->second;
    for (const auto& clip : state.displayClips) {
        if (clip.clipId != currentClipId && clip.clipId > 0) {
            options.push_back(clip.clipId);
        }
    }

    std::sort(options.begin(), options.end());
    return options;
}

}
