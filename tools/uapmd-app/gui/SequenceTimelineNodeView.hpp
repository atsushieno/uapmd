#pragma once

#include <memory>

#include <imgui.h>

#include <TimelineViews/INodeView.h>

class SequenceTimelineNodeView : public INodeView {
public:
    void PreDraw() override { nodesDrawSkipped_ = 0; }
    void DrawNodeView(const ImRect& area, const sTimelineSection& timeline, ImTimeline::Timeline* context) override;
    void defaultNodeDraw(const ImRect& area, const TimelineNode& node, ImTimeline::Timeline* context) override;
    void PerformanceDebugUI() const override;

private:
    int nodesDrawSkipped_ = 0;
};
