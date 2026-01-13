#include "SequenceTimelineNodeView.hpp"

#include <algorithm>

#include <Timeline.h>
#include <TimelineData/ImDataController.h>
#include <TimelineCore/TimelineDefines.h>
#include <TimelineCore/TimelineTimeStep.h>
#include <TimelineViews/HorizontalNodeView.h> // for utility macros? maybe not
#include <Core/ImTimelineUtility.h>

void SequenceTimelineNodeView::DrawNodeView(const ImRect& area, const sTimelineSection& timeline, ImTimeline::Timeline* context) {
    if (!timeline.mbIsInitialized || context == nullptr) {
        return;
    }

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 canvasSize = context->mContentAreaRect.GetSize();
    ImVec2 contentMin = context->mContentAreaRect.Min;

    auto* nodeData = timeline.mNodeData;
    if (!nodeData) {
        return;
    }

    ImU32 bgColor = IM_COL32(40, 50, 50, 255);
    if (area.Contains(context->GetLastInputData().MousePos)) {
        bgColor += 0x80201008;
        context->SetSelectedTimeline(timeline.mID);
    }
    drawList->AddRectFilled(area.Min, area.Max, bgColor, 0.0f);

    // Legend
    std::string label;
    ImTimelineUtility::sprint_f(label, "[%d] (%s)", timeline.mID, timeline.mProps.mSectionName.c_str());
    drawList->AddText(ImVec2(area.Min.x + 3.0f, area.Min.y), 0xFFFFFFFF, label.c_str());

    ImRect panelRect(area.Min, area.Max);
    panelRect.Min.x += context->mStyle.LegendWidth - (context->GetStartTimestamp() * context->GetScale());
    panelRect.Max.x -= context->mStyle.LegendWidth - (context->GetStartTimestamp() * context->GetScale());

    ImRect panelAbsolute(area.Min, area.Max);
    panelAbsolute.Min.x += context->mStyle.LegendWidth;

    drawList->PushClipRect(panelAbsolute.Min, panelAbsolute.Max, true);

    size_t sectionHeight = timeline.mProps.mDisplayProperties.mHeight;
    nodeData->iterate([&](TimelineNode& node) {
        if (!node.mFlags.test(eTimelineNodeFlags::TimelineNodeFlags_AutofitHeight)) {
            sectionHeight = node.displayProperties.mHeight;
        }

        if (context->IsDragging() && context->mDragData.DragNode.GetID() == node.GetID()) {
            return;
        }

        const float scale = static_cast<float>(context->GetScale());
        ImVec2 slotP1(panelRect.Min.x + node.start * scale, panelRect.Min.y);
        ImVec2 slotP2(panelRect.Min.x + node.end * scale + scale, slotP1.y + sectionHeight - node.displayProperties.AccentThickness);
        ImRect nodeRect(slotP1, slotP2);

        const bool isVisible = slotP1.x <= (canvasSize.x + contentMin.x) &&
                               slotP2.x >= (contentMin.x + context->mStyle.LegendWidth);
        if (!isVisible) {
            ++nodesDrawSkipped_;
            return;
        }

        defaultNodeDraw(nodeRect, node, context);

        const bool hovered = nodeRect.Contains(context->GetLastInputData().MousePos) &&
                             context->GetLastInputData().LeftMouseDown;
        const bool inputDelay = (context->GetLastInputData().MouseDownDuration > 40.0f);

        if (hovered && context->mDragData.DragState == eDragState::None) {
            context->SelectNode(&node);
        }

        if (hovered && !context->IsDragging() && inputDelay) {
            context->mDragData.DragState = eDragState::DragNode;
            context->mDragData.DragNode = *context->GetSelectedNode();
            context->mDragData.DragStartMouseDelta = context->GetLastInputData().MousePos - slotP1;
            context->mDragData.DragRect = nodeRect;
        }
    });

    drawList->PopClipRect();

    if (context->IsDragging() && context->GetSelectedSection() == timeline.mID) {
        drawList->AddRect(area.Min, area.Max, ImTimelineUtility::Color::Yellow, 0.0f);
    }
}

void SequenceTimelineNodeView::defaultNodeDraw(const ImRect& area, const TimelineNode& node, ImTimeline::Timeline* context) {
    if (!context) {
        return;
    }

    bool isSelected = context->GetSelectedNode() == &node;
    auto* drawList = ImGui::GetWindowDrawList();

    ImU32 bgColor = node.displayProperties.mBackgroundColor;
    ImU32 bgColor2 = node.displayProperties.mBackgroundColorTwo;
    if (node.mFlags.test(eTimelineNodeFlags::TimelineNodeFlags_UseSectionBackground)) {
        const auto& props = context->GetSectionDisplayProperties(node.GetSection());
        bgColor = props.mBackgroundColor;
        bgColor2 = props.mBackgroundColorTwo;
    }
    if (area.Contains(context->GetLastInputData().MousePos)) {
        bgColor += 0x00201000;
    }

    ImVec2 min = area.Min;
    ImVec2 max = area.Max;

    drawList->AddRectFilledMultiColor(min, max, bgColor, bgColor, bgColor2, bgColor2);
    drawList->AddText(min + ImVec2(node.displayProperties.AccentThickness, node.displayProperties.AccentThickness),
                      node.displayProperties.mForegroundColor, node.displayText.c_str());

    const float borderThickness = node.displayProperties.BorderThickness;
    const ImU32 fgColor = isSelected ? context->mStyle.SelectedNodeOutlineColor : node.displayProperties.mForegroundColor;
    drawList->AddRect(min, max - ImVec2(borderThickness, borderThickness), fgColor,
                      node.displayProperties.BorderRadius, 0, borderThickness);
}

void SequenceTimelineNodeView::PerformanceDebugUI() const {
    ImGui::Text("Nodes Draw Skipped: %d", nodesDrawSkipped_);
}
