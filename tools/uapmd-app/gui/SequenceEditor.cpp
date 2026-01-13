#include "SequenceEditor.hpp"

#include <algorithm>
#include <format>
#include <iostream>
#include <map>

#include <imgui.h>
#include <ImTimeline.h>
#include <TimelineData/ImDataController.h>

#include "../AppModel.hpp"
#include "SequenceTimelineNodeView.hpp"

namespace uapmd::gui {

SequenceEditor::~SequenceEditor() = default;
SequenceEditor::SequenceEditorState::~SequenceEditorState() = default;

namespace {

constexpr int32_t kTimelineSectionId = 0;

} // namespace

void SequenceEditor::showWindow(int32_t trackIndex) {
    auto [it, inserted] = windows_.try_emplace(trackIndex);
    it->second.visible = true;
    if (inserted) {
        it->second.timelineDirty = true;
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
        it->second.timelineDirty = true;
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

        const float reservedHeight = ImGui::GetFrameHeightWithSpacing() * 2.0f;
        float tabRegionHeight = ImGui::GetContentRegionAvail().y - reservedHeight;
        const float minTabHeight = 150.0f * context.uiScale;
        tabRegionHeight = std::max(minTabHeight, tabRegionHeight);

        std::string tabBarId = std::format("SequenceEditorTabs##{}", trackIndex);
        if (ImGui::BeginTabBar(tabBarId.c_str())) {
            if (ImGui::BeginTabItem("Timeline")) {
                renderTimelineEditor(trackIndex, state, context, tabRegionHeight);
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Clip Table")) {
                renderClipTable(trackIndex, state, context, tabRegionHeight);
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }

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

void SequenceEditor::renderClipTable(int32_t trackIndex, SequenceEditorState& state, const RenderContext& context, float availableHeight) {
    if (availableHeight <= 0.0f) {
        availableHeight = ImGui::GetContentRegionAvail().y;
    }
    availableHeight = std::max(availableHeight, 0.0f);

    std::string childId = std::format("##ClipTableRegion{}", trackIndex);
    if (ImGui::BeginChild(childId.c_str(), ImVec2(0, availableHeight), false, ImGuiWindowFlags_None)) {
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

void SequenceEditor::renderTimelineEditor(int32_t trackIndex, SequenceEditorState& state, const RenderContext& context, float availableHeight) {
    if (availableHeight <= 0.0f) {
        availableHeight = ImGui::GetContentRegionAvail().y;
    }
    availableHeight = std::max(availableHeight, 0.0f);

    const float infoHeight = ImGui::GetTextLineHeightWithSpacing();
    float timelineHeight = availableHeight - infoHeight - ImGui::GetStyle().ItemSpacing.y;
    if (timelineHeight <= 0.0f) {
        timelineHeight = availableHeight;
    }

    ImGui::TextDisabled("Timeline units: seconds");
    ImGui::Spacing();

    if (state.timelineDirty) {
        rebuildTimelineModel(trackIndex, state, context);
    }

    if (!state.timeline) {
        ImGui::TextUnformatted("Unable to build timeline for this track.");
        return;
    }

    timelineHeight = std::max(timelineHeight, 120.0f * context.uiScale);

    std::string timelineChildId = std::format("##TimelineRegion{}", trackIndex);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    if (ImGui::BeginChild(timelineChildId.c_str(), ImVec2(0, timelineHeight), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        if (state.timeline->mDragData.DragState == eDragState::DragNode && state.activeDragSection == -1) {
            state.activeDragSection = state.timeline->mDragData.DragNode.GetSection();
        }

        state.timeline->DrawTimeline();

        if (state.displayClips.empty()) {
            ImGui::SetCursorPos(ImVec2(8.0f, 8.0f));
            ImGui::TextDisabled("Add clips to view them on the timeline.");
        }

        if (state.activeDragSection != -1 &&
            state.timeline->mDragData.DragState == eDragState::None &&
            !state.timeline->IsDragging()) {
            int section = state.activeDragSection;
            state.activeDragSection = -1;

            auto it = state.sectionToClip.find(section);
            if (it != state.sectionToClip.end() && it->second >= 0) {
                const auto& timelineSection = state.timeline->GetTimelineSection(section);
                double newStart = 0.0;
                if (timelineSection.mNodeData) {
                    timelineSection.mNodeData->iterate([&](TimelineNode& node) {
                        newStart = static_cast<double>(node.start);
                    });
                }
                if (context.moveClipAbsolute) {
                    context.moveClipAbsolute(trackIndex, it->second, newStart);
                }
            }
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

void SequenceEditor::rebuildTimelineModel(int32_t trackIndex, SequenceEditorState& state, const RenderContext& context) {
    state.timeline = std::make_unique<ImTimeline::Timeline>();

    if (!state.timeline) {
        state.timelineDirty = false;
        return;
    }

    if (!state.timelineView) {
        state.timelineView = std::make_shared<SequenceTimelineNodeView>();
    }

    ImTimelineStyle style;
    style.LegendWidth = 140.0f * context.uiScale;
    style.HeaderHeight = static_cast<int>(24.0f * context.uiScale);
    style.ScrollbarThickness = static_cast<int>(12.0f * context.uiScale);
    style.SeekbarWidth = 2.0f * context.uiScale;
    state.timeline->SetTimelineStyle(style);

    const float baseHeight = std::max(80.0f * context.uiScale, 40.0f);
    int32_t maxFrame = 0;
    int sectionId = 0;
    state.sectionToClip.clear();
    state.activeDragSection = -1;

    for (const auto& clip : state.displayClips) {
        const int currentSection = sectionId++;
        std::string sectionName = clip.name.empty() ? std::format("Clip {}", clip.clipId) : clip.name;
        state.timeline->InitializeTimelineSection(currentSection, sectionName);
        state.timeline->SetTimelineName(currentSection, sectionName);

        auto& props = state.timeline->GetSectionDisplayProperties(currentSection);
        props.mHeight = baseHeight;
        props.mBackgroundColor = IM_COL32(63, 76, 107, 200);
        props.mBackgroundColorTwo = IM_COL32(43, 54, 86, 200);
        props.mForegroundColor = IM_COL32(255, 255, 255, 255);
        props.BorderRadius = 6.0f * context.uiScale;
        props.BorderThickness = 1.0f;
        props.AccentThickness = 8;
        state.timeline->SetTimelineHeight(currentSection, props.mHeight);

        TimelineNode node;
        node.Setup(currentSection, clip.timelineStart, clip.timelineEnd, sectionName);
        node.displayProperties = props;
        state.timeline->AddNewNode(&node);

        maxFrame = std::max(maxFrame, clip.timelineEnd);
        state.sectionToClip[currentSection] = clip.clipId;
    }

    if (sectionId == 0) {
        state.timeline->InitializeTimelineSection(0, std::format("Track {}", trackIndex));
        state.timeline->SetTimelineHeight(0, baseHeight);
        state.sectionToClip[0] = -1;
    }

    if (maxFrame <= 0) {
        maxFrame = 1000;
    }

    state.timeline->SetStartFrame(0);
    state.timeline->SetMaxFrame(maxFrame + 200);
    state.timeline->SetNodeViewUI(state.timelineView);
    state.timeline->SetScale(std::max(1.0f, 5.0f * context.uiScale));
    state.timelineDirty = false;
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
    const bool anchorChanged = renderAnchorCombo(trackIndex, clip, context);

    // Origin column
    ImGui::TableSetColumnIndex(1);
    const bool originChanged = renderOriginCombo(trackIndex, clip, context);

    // Position column
    ImGui::TableSetColumnIndex(2);
    const bool positionChanged = renderPositionInput(trackIndex, clip, context);

    // Name column (editable)
    ImGui::TableSetColumnIndex(3);
    const bool nameChanged = renderNameInput(trackIndex, clip, context);

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

    if (anchorChanged || originChanged || positionChanged || nameChanged) {
        auto refresh = context.refreshClips;
        if (refresh) {
            refresh(trackIndex);
        }
    }
}

bool SequenceEditor::renderAnchorCombo(int32_t trackIndex, const ClipRow& clip, const RenderContext& context) {
    bool changed = false;
    std::string anchorLabel = "Track";
    if (clip.anchorClipId != -1) {
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

    ImGui::SetNextItemWidth(-FLT_MIN);
    std::string comboId = std::format("##AnchorCombo{}", clip.clipId);
    if (ImGui::BeginCombo(comboId.c_str(), anchorLabel.c_str())) {
        // Track anchor option
        bool isTrackAnchor = (clip.anchorClipId == -1);
        if (ImGui::Selectable("Track", isTrackAnchor)) {
            if (context.updateClip) {
                context.updateClip(trackIndex, clip.clipId, -1, clip.anchorOrigin, clip.position);
                changed = true;
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
                        changed = true;
                    }
                }
            }
        }

        ImGui::EndCombo();
    }

    return changed;
}

bool SequenceEditor::renderOriginCombo(int32_t trackIndex, const ClipRow& clip, const RenderContext& context) {
    bool changed = false;
    ImGui::SetNextItemWidth(-FLT_MIN);
    std::string originComboId = std::format("##OriginCombo{}", clip.clipId);
    if (ImGui::BeginCombo(originComboId.c_str(), clip.anchorOrigin.c_str())) {
        bool isStart = (clip.anchorOrigin == "Start");
        if (ImGui::Selectable("Start", isStart)) {
            if (context.updateClip) {
                context.updateClip(trackIndex, clip.clipId, clip.anchorClipId, "Start", clip.position);
                changed = true;
            }
        }

        bool isEnd = (clip.anchorOrigin == "End");
        if (ImGui::Selectable("End", isEnd)) {
            if (context.updateClip) {
                context.updateClip(trackIndex, clip.clipId, clip.anchorClipId, "End", clip.position);
                changed = true;
            }
        }

        ImGui::EndCombo();
    }
    return changed;
}

bool SequenceEditor::renderPositionInput(int32_t trackIndex, const ClipRow& clip, const RenderContext& context) {
    bool changed = false;
    char posBuffer[64];
    strncpy(posBuffer, clip.position.c_str(), sizeof(posBuffer) - 1);
    posBuffer[sizeof(posBuffer) - 1] = '\0';

    ImGui::SetNextItemWidth(-FLT_MIN);
    std::string inputId = std::format("##PosInput{}", clip.clipId);
    if (ImGui::InputText(inputId.c_str(), posBuffer, sizeof(posBuffer), ImGuiInputTextFlags_EnterReturnsTrue)) {
        if (context.updateClip) {
            context.updateClip(trackIndex, clip.clipId, clip.anchorClipId, clip.anchorOrigin, std::string(posBuffer));
            changed = true;
        }
    }
    return changed;
}

bool SequenceEditor::renderNameInput(int32_t trackIndex, const ClipRow& clip, const RenderContext& context) {
    bool changed = false;
    static std::map<int32_t, std::array<char, 256>> nameBuffers;
    if (nameBuffers.find(clip.clipId) == nameBuffers.end()) {
        nameBuffers[clip.clipId] = {};
        strncpy(nameBuffers[clip.clipId].data(), clip.name.c_str(), nameBuffers[clip.clipId].size() - 1);
    }

    ImGui::SetNextItemWidth(-FLT_MIN);
    std::string nameInputId = std::format("##NameInput{}", clip.clipId);
    if (ImGui::InputText(nameInputId.c_str(), nameBuffers[clip.clipId].data(),
                        nameBuffers[clip.clipId].size(), ImGuiInputTextFlags_EnterReturnsTrue)) {
        if (context.updateClipName) {
            context.updateClipName(trackIndex, clip.clipId, std::string(nameBuffers[clip.clipId].data()));
            changed = true;
        }
    }
    return changed;
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
