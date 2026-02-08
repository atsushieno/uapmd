#include "SequenceEditor.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <iostream>
#include <map>
#include <unordered_map>
#include <unordered_set>

#include <imgui.h>
#include <ImTimeline.h>

#include "../AppModel.hpp"
#include "ClipPreview.hpp"

namespace uapmd::gui {

namespace {

constexpr int32_t kTimelineSectionId = 0;
constexpr float kTimelineSectionSpacing = 5.0f; // Matches Timeline::DrawTimeline spacing
constexpr float kTimelineChildPadding = 8.0f;   // Matches Timeline::DrawTimeline padding
constexpr ImU32 kTimelinePlayheadColor = IM_COL32(255, 230, 0, 255);
} // namespace

SequenceEditor::~SequenceEditor() = default;

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
    auto& state = windows_[trackIndex];
    state.displayClips = clips;
    state.timelineDirty = true;
    pruneClipPreviewCache(state);
}

void SequenceEditor::removeStaleWindows(int32_t maxValidTrackIndex) {
    // Remove windows for track indices that no longer exist
    for (auto it = windows_.begin(); it != windows_.end();) {
        if (it->first > maxValidTrackIndex) {
            it = windows_.erase(it);
        } else {
            ++it;
        }
    }
}

void SequenceEditor::renderTimelineInline(int32_t trackIndex, const RenderContext& context, float availableHeight) {
    auto it = windows_.find(trackIndex);
    if (it == windows_.end()) {
        return;
    }
    renderTimelineContent(trackIndex, it->second, context, availableHeight, false);
}

float SequenceEditor::getInlineTimelineHeight(int32_t trackIndex, float uiScale) const {
    const float baseSectionHeight = std::max(80.0f * uiScale, 40.0f);
    const float minHeight = 120.0f * uiScale;

    size_t sectionCount = 1;
    ImTimelineStyle style;
    auto it = windows_.find(trackIndex);
    if (it != windows_.end()) {
        sectionCount = std::max<size_t>(1, it->second.displayClips.size());
        style = it->second.timelineStyle;
    } else {
        style.LegendWidth = 140.0f * uiScale;
        style.HeaderHeight = static_cast<int>(24.0f * uiScale);
        style.ScrollbarThickness = static_cast<int>(12.0f * uiScale);
        style.SeekbarWidth = 2.0f * uiScale;
    }

    const float headerHeight = static_cast<float>(style.HeaderHeight);
    const float scrollbarHeight = style.HasScrollbar ? static_cast<float>(style.ScrollbarThickness) : 0.0f;
    const float sectionsHeight = static_cast<float>(sectionCount) * baseSectionHeight;
    const float spacingHeight = static_cast<float>(sectionCount) * kTimelineSectionSpacing;
    const float computedHeight = headerHeight + sectionsHeight + spacingHeight + kTimelineChildPadding + scrollbarHeight;

    return std::max(computedHeight, minHeight);
}

void SequenceEditor::reset() {
    windows_.clear();
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

        float clipRegionHeight = ImGui::GetContentRegionAvail().y;
        clipRegionHeight = std::max(150.0f * context.uiScale, clipRegionHeight);
        renderClipTable(trackIndex, state, context, clipRegionHeight);

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

void SequenceEditor::renderTimelineContent(int32_t trackIndex, SequenceEditorState& state, const RenderContext& context, float availableHeight, bool showLabel) {
    if (availableHeight <= 0.0f) {
        availableHeight = ImGui::GetContentRegionAvail().y;
    }
    availableHeight = std::max(availableHeight, 0.0f);

    float timelineHeight = availableHeight;
    if (showLabel) {
        const float infoHeight = ImGui::GetTextLineHeightWithSpacing();
        timelineHeight -= infoHeight + ImGui::GetStyle().ItemSpacing.y;
        timelineHeight = std::max(timelineHeight, 0.0f);
        ImGui::TextDisabled("Timeline units: seconds");
        ImGui::Spacing();
    }

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
        // Check if this window is actually hovered (will be false if another window is on top)
        const bool timelineHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
        const ImVec2 childWindowPos = ImGui::GetWindowPos();
        const ImVec2 childWindowSize = ImGui::GetWindowSize();
        const float clipAreaMinX = childWindowPos.x + state.timelineStyle.LegendWidth;
        float clipAreaMinY = childWindowPos.y + static_cast<float>(state.timelineStyle.HeaderHeight);
        const float clipAreaMaxX = childWindowPos.x + childWindowSize.x;
        float clipAreaMaxY = childWindowPos.y + childWindowSize.y;
        if (state.timelineStyle.HasScrollbar) {
            clipAreaMaxY -= static_cast<float>(state.timelineStyle.ScrollbarThickness);
        }

        // Block mouse input to ImTimeline when there's a popup/modal window on top
        // ImTimeline reads directly from ImGui::GetIO(), so we temporarily clear mouse state
        // Check if any popup is open that could be blocking this timeline
        const bool popupBlocking = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);

        ImGuiIO& io = ImGui::GetIO();
        bool savedMouseDown[5];
        float savedMouseWheel = 0.0f;
        float savedMouseWheelH = 0.0f;

        if (popupBlocking) {
            // Save and clear mouse state to prevent ImTimeline from reacting
            for (int i = 0; i < 5; ++i) {
                savedMouseDown[i] = io.MouseDown[i];
                io.MouseDown[i] = false;
            }
            savedMouseWheel = io.MouseWheel;
            savedMouseWheelH = io.MouseWheelH;
            io.MouseWheel = 0.0f;
            io.MouseWheelH = 0.0f;
        }

        state.timeline->DrawTimeline();

        // Restore mouse state
        if (popupBlocking) {
            for (int i = 0; i < 5; ++i)
                io.MouseDown[i] = savedMouseDown[i];
            io.MouseWheel = savedMouseWheel;
            io.MouseWheelH = savedMouseWheelH;
        }

        const float headerMinX = clipAreaMinX;
        const float headerMaxX = clipAreaMaxX;
        const float headerMinY = childWindowPos.y;
        const float headerMaxY = clipAreaMinY;
        if (headerMaxX > headerMinX && headerMaxY > headerMinY) {
            drawPlayheadIndicator(state, headerMinX, headerMinY, headerMaxX, headerMaxY);
        }

        // Only start tracking drag if no popup is blocking and timeline is hovered
        if (state.timeline->mDragData.DragState == eDragState::DragNode &&
            state.activeDragSection == -1 &&
            !popupBlocking &&
            timelineHovered) {
            state.activeDragSection = state.timeline->mDragData.DragNode.GetSection();
        }

        // Cancel active drag if a popup appeared on top
        if (state.activeDragSection != -1 && popupBlocking) {
            state.activeDragSection = -1;
        }

        int32_t requestedContextClip = -1;
        const ImVec2 mousePos = ImGui::GetMousePos();
        const bool mouseInClipArea =
            mousePos.x >= clipAreaMinX && mousePos.x <= clipAreaMaxX &&
            mousePos.y >= clipAreaMinY && mousePos.y <= clipAreaMaxY;
        if (timelineHovered && mouseInClipArea && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            int hoveredSection = state.timeline->GetSelectedSection();
            auto clipIt = state.sectionToClip.find(hoveredSection);
            if (clipIt != state.sectionToClip.end() && clipIt->second >= 0) {
                requestedContextClip = clipIt->second;
            }
        }


        if (state.activeDragSection != -1 &&
            state.timeline->mDragData.DragState == eDragState::None &&
            !state.timeline->IsDragging()) {
            int section = state.activeDragSection;
            state.activeDragSection = -1;

            auto it = state.sectionToClip.find(section);
            if (it != state.sectionToClip.end() && it->second >= 0) {
                // Use stored NodeID to find the node and get its new position
                auto nodeIdIt = state.sectionToNodeId.find(section);
                if (nodeIdIt != state.sectionToNodeId.end()) {
                    auto* node = state.timeline->FindNodeByNodeID(nodeIdIt->second);
                    if (node) {
                        double newStart = static_cast<double>(node->start);
                        if (context.moveClipAbsolute) {
                            context.moveClipAbsolute(trackIndex, it->second, newStart);
                        }
                    }
                }
            }
        }

        std::string popupId = std::format("TimelineClipContext##{}", trackIndex);
        if (requestedContextClip >= 0) {
            state.contextMenuClipId = requestedContextClip;
            ImGui::OpenPopup(popupId.c_str());
        }

        if (ImGui::BeginPopup(popupId.c_str())) {
            const ClipRow* contextClip = nullptr;
            if (state.contextMenuClipId >= 0) {
                auto clipRowIt = std::find_if(
                    state.displayClips.begin(),
                    state.displayClips.end(),
                    [clipId = state.contextMenuClipId](const ClipRow& row) { return row.clipId == clipId; }
                );
                if (clipRowIt != state.displayClips.end()) {
                    contextClip = &(*clipRowIt);
                }
            }

            if (!contextClip) {
                ImGui::TextDisabled("Clip not available.");
            } else {
                const bool canShowDump = contextClip->isMidiClip && static_cast<bool>(context.showMidiClipDump);
                if (!canShowDump) {
                    ImGui::BeginDisabled();
                }
                if (ImGui::MenuItem("Show Dump List")) {
                    if (context.showMidiClipDump) {
                        context.showMidiClipDump(trackIndex, contextClip->clipId);
                    }
                    ImGui::CloseCurrentPopup();
                }
                if (!canShowDump) {
                    ImGui::EndDisabled();
                }

                const bool canDelete = static_cast<bool>(context.removeClip);
                if (!canDelete) {
                    ImGui::BeginDisabled();
                }
                if (ImGui::MenuItem("Delete")) {
                    if (context.removeClip) {
                        context.removeClip(trackIndex, contextClip->clipId);
                    }
                    ImGui::CloseCurrentPopup();
                }
                if (!canDelete) {
                    ImGui::EndDisabled();
                }
            }

            ImGui::EndPopup();
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

    ImTimelineStyle style;
    style.LegendWidth = 140.0f * context.uiScale;
    style.HeaderHeight = static_cast<int>(24.0f * context.uiScale);
    style.ScrollbarThickness = static_cast<int>(12.0f * context.uiScale);
    style.SeekbarWidth = 2.0f * context.uiScale;
    style.HasSeekbar = false;
    state.timeline->SetTimelineStyle(style);
    state.timelineStyle = style;

    const float baseHeight = std::max(80.0f * context.uiScale, 40.0f);
    int32_t maxFrame = 0;
    int sectionId = 0;
    state.sectionToClip.clear();
    state.sectionToNodeId.clear();
    state.activeDragSection = -1;

    for (const auto& clip : state.displayClips) {
        const int currentSection = sectionId++;
        std::string sectionName = std::format("Track {}", trackIndex + 1);
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
        auto preview = ensureClipPreview(trackIndex, clip, state);
        auto clipLabel = clip.name.empty() ? std::format("Clip {}", clip.clipId) : clip.name;
        auto customNode = createClipContentNode(preview, context.uiScale, clipLabel);
        if (customNode) {
            node.InitalizeCustomNode(customNode);
        }
        auto* addedNode = state.timeline->AddNewNode(&node);
        if (addedNode) {
            state.sectionToNodeId[currentSection] = addedNode->GetID();
        }

        maxFrame = std::max(maxFrame, clip.timelineEnd);
        state.sectionToClip[currentSection] = clip.clipId;
    }

    if (sectionId == 0) {
        state.timeline->InitializeTimelineSection(0, std::format("Track {}", trackIndex + 1));
        state.timeline->SetTimelineHeight(0, baseHeight);
        state.sectionToClip[0] = -1;
    }

    if (maxFrame <= 0) {
        maxFrame = 1000;
    }

    state.timeline->SetStartFrame(0);
    state.timeline->SetMaxFrame(maxFrame + 200);
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

void SequenceEditor::drawPlayheadIndicator(
    const SequenceEditorState& state,
    float headerMinX,
    float headerMinY,
    float headerMaxX,
    float headerMaxY
) const {
    if (!state.timeline) {
        return;
    }

    auto& appModel = uapmd::AppModel::instance();
    const int32_t sampleRate = appModel.sampleRate();
    if (sampleRate <= 0) {
        return;
    }

    const auto& timelineState = appModel.timeline();
    const double playheadSeconds = timelineState.playheadPosition.toSeconds(sampleRate);
    if (!std::isfinite(playheadSeconds)) {
        return;
    }

    const int32_t maxFrame = state.timeline->GetMaxFrame();
    if (maxFrame <= 0) {
        return;
    }

    const double clampedFrame = std::clamp(playheadSeconds, 0.0, static_cast<double>(maxFrame));
    const double startFrame = static_cast<double>(state.timeline->GetStartTimestamp());
    if (clampedFrame < startFrame || clampedFrame > static_cast<double>(maxFrame)) {
        return;
    }

    const float scale = state.timeline->GetScale();
    const float clipMinX = headerMinX;
    const float clipMaxX = headerMaxX;
    if (clipMaxX <= clipMinX) {
        return;
    }

    const float x = clipMinX + static_cast<float>((clampedFrame - startFrame) * static_cast<double>(scale)) + (scale * 0.5f);
    const float yTop = headerMinY;
    const float yBottom = headerMaxY;
    if (yBottom <= yTop) {
        return;
    }

    const float headerHeight = yBottom - yTop;
    const float triangleHeight = std::clamp(headerHeight * 0.75f, 6.0f, headerHeight);
    const float halfBaseWidth = std::clamp(triangleHeight * 0.35f, 4.0f, 12.0f);

    const float tipX = std::clamp(x, clipMinX, clipMaxX);
    const float baseLeftX = std::max(clipMinX, tipX - halfBaseWidth);
    const float baseRightX = std::min(clipMaxX, tipX + halfBaseWidth);
    const float tipY = std::min(yBottom, yTop + triangleHeight);

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->PushClipRect(ImVec2(clipMinX, yTop), ImVec2(clipMaxX, yBottom), true);
    const ImVec2 baseLeft(baseLeftX, yTop);
    const ImVec2 baseRight(baseRightX, yTop);
    const ImVec2 tip(tipX, tipY);
    drawList->AddTriangleFilled(baseLeft, baseRight, tip, kTimelinePlayheadColor);
    drawList->AddTriangle(baseLeft, baseRight, tip, IM_COL32(0, 0, 0, 200), 1.0f);
    drawList->PopClipRect();
}

void SequenceEditor::pruneClipPreviewCache(SequenceEditorState& state) {
    std::unordered_set<int32_t> validIds;
    validIds.reserve(state.displayClips.size());
    for (const auto& clip : state.displayClips) {
        validIds.insert(clip.clipId);
    }

    for (auto it = state.clipPreviews.begin(); it != state.clipPreviews.end();) {
        if (validIds.find(it->first) == validIds.end()) {
            it = state.clipPreviews.erase(it);
        } else {
            ++it;
        }
    }
}

const uapmd::ClipData* SequenceEditor::findClipData(int32_t trackIndex, int32_t clipId) const {
    auto tracks = uapmd::AppModel::instance().getTimelineTracks();
    if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(tracks.size())) {
        return nullptr;
    }
    auto* track = tracks[trackIndex];
    if (!track) {
        return nullptr;
    }
    return track->clipManager().getClip(clipId);
}

std::string SequenceEditor::buildClipSignature(const ClipRow& clip, const uapmd::ClipData* clipData) const {
    std::string sourcePath;
    if (clipData && !clipData->filepath.empty()) {
        sourcePath = clipData->filepath;
    } else {
        sourcePath = clip.filepath;
    }
    if (sourcePath.empty()) {
        sourcePath = clip.name;
    }

    const int64_t durationSamples = clipData ? clipData->durationSamples : 0;
    const int32_t sourceNodeId = clipData ? clipData->sourceNodeInstanceId : -1;
    return std::format("{}|{}|{}|{}|{}",
                       sourcePath,
                       clip.isMidiClip ? 'm' : 'a',
                       clip.timelineEnd - clip.timelineStart,
                       durationSamples,
                       sourceNodeId);
}

std::shared_ptr<ClipPreview> SequenceEditor::ensureClipPreview(
    int32_t trackIndex,
    const ClipRow& clip,
    SequenceEditorState& state
) {
    const auto* clipData = findClipData(trackIndex, clip.clipId);
    const auto signature = buildClipSignature(clip, clipData);
    auto existingIt = state.clipPreviews.find(clip.clipId);
    if (existingIt != state.clipPreviews.end() &&
        existingIt->second &&
        existingIt->second->signature == signature) {
        existingIt->second->displayName = clip.name;
        return existingIt->second;
    }

    const double fallbackDurationSeconds = std::max(
        0.0,
        static_cast<double>(clip.timelineEnd - clip.timelineStart)
    );

    auto makeErrorPreview = [&](const std::string& message) {
        auto preview = std::make_shared<ClipPreview>();
        preview->isMidiClip = clip.isMidiClip;
        preview->hasError = true;
        preview->errorMessage = message;
        preview->clipDurationSeconds = fallbackDurationSeconds;
        return preview;
    };

    std::shared_ptr<ClipPreview> preview;
    if (clip.isMidiClip) {
        if (clipData) {
            preview = createMidiClipPreview(trackIndex, *clipData, fallbackDurationSeconds);
        } else {
            preview = makeErrorPreview("Clip data unavailable");
        }
    } else {
        std::string filepath = (clipData && !clipData->filepath.empty())
            ? clipData->filepath
            : clip.filepath;
        preview = createAudioClipPreview(filepath, fallbackDurationSeconds, clipData);
    }

    if (!preview) {
        preview = makeErrorPreview("Preview unavailable");
    }

    preview->signature = signature;
    preview->displayName = clip.name;
    state.clipPreviews[clip.clipId] = preview;
    return preview;
}

}
