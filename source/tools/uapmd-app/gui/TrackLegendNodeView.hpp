#pragma once

#include <functional>
#include <imgui.h>
#include <imgui_internal.h>
#include <HorizontalNodeView.h>

namespace uapmd::gui {

class TrackLegendNodeView : public HorizontalNodeView {
public:
    int32_t trackIndex = -1;
    std::function<void(int32_t trackIndex, const ImRect& legendArea)> renderContent;

    void DrawLegendArea(const sTimelineSection& /*section*/, ImTimeline::Timeline* ctx, const ImRect& area) override {
        if (!renderContent)
            return;
        ImRect legendArea(area.Min, ImVec2(area.Min.x + ctx->mStyle.LegendWidth, area.Max.y));
        renderContent(trackIndex, legendArea);
    }
};

} // namespace uapmd::gui
