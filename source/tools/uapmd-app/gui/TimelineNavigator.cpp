#include "TimelineNavigator.hpp"

#include <algorithm>
#include <cmath>

#include <imgui.h>
#include <ImTimeline.h>

namespace uapmd::gui {

void renderTimelineNavigator(ImTimeline::Timeline& timeline, bool& hasExplicitZoom,
                             float uiScale, float barStartScreenX,
                             double contentFrames, double playheadFrame,
                             float visibleWidthPixels,
                             const std::vector<NavigatorClip>& clips, int rowCount) {
    const float rowHeight = kNavigatorHeightPt * uiScale;
    const ImGuiStyle& style = ImGui::GetStyle();
    ImGuiIO& io = ImGui::GetIO();

    ImGui::PushID("TimelineNavigator");

    // Zoom slider, vertically centered in the row, filling the space up to the controller's
    // anchored left edge. Logarithmic: the range spans four orders of magnitude, so linear
    // would cram all useful zoom-out territory into the first few pixels.
    const ImVec2 rowStart = ImGui::GetCursorScreenPos();
    const float sliderHeight = ImGui::GetFrameHeight();
    ImGui::SetCursorScreenPos(ImVec2(rowStart.x, rowStart.y + (rowHeight - sliderHeight) * 0.5f));
    float scale = timeline.GetScale();
    ImGui::SetNextItemWidth(std::max(40.0f, barStartScreenX - rowStart.x - style.ItemSpacing.x));
    if (ImGui::SliderFloat("##Zoom", &scale, kMinSafeTimelineScale, kMaxTimelineScale, "%.2f",
                           ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp)) {
        timeline.SetScale(scale);
        hasExplicitZoom = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Timeline zoom");

    // Position controller bar filling the rest of the row, aligned with the clip area.
    ImGui::SameLine();
    ImGui::SetCursorScreenPos(ImVec2(barStartScreenX, rowStart.y));
    const float barWidth = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const ImVec2 barMin = ImGui::GetCursorScreenPos();
    const ImVec2 barMax(barMin.x + barWidth, barMin.y + rowHeight);
    ImGui::InvisibleButton("##PositionController", ImVec2(barWidth, rowHeight));
    const bool hovered = ImGui::IsItemHovered();

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(barMin, barMax, ImGui::GetColorU32(ImGuiCol_FrameBg), 4.0f);

    // Re-read in case the slider changed it this frame. The bar's domain is the song content
    // length, NOT timeline.GetMaxFrame(): ImTimeline inflates mFrameMax when zoomed out past
    // the content and never shrinks it back, which would permanently squash the region rect.
    scale = timeline.GetScale();
    const double domain = contentFrames;
    const double visibleFrames = (scale > 0.0f && visibleWidthPixels > 0.0f)
        ? static_cast<double>(visibleWidthPixels) / scale
        : domain;

    if (domain > 0.0) {
        // Whole-song overview: one lane per track, each clip a small bar, drawn underneath the
        // translucent visible-region rectangle so the region stays readable on top of them.
        if (rowCount > 0 && !clips.empty()) {
            const float lanePad = 3.0f * uiScale;
            const float laneAreaTop = barMin.y + lanePad;
            const float laneHeight = (rowHeight - 2.0f * lanePad) / static_cast<float>(rowCount);
            const ImU32 clipColor = ImGui::GetColorU32(ImGuiCol_PlotHistogram, 0.75f);
            for (const auto& clip : clips) {
                if (clip.end <= clip.start || clip.row < 0 || clip.row >= rowCount)
                    continue;
                const float cx0 = barMin.x + static_cast<float>(std::clamp(clip.start / domain, 0.0, 1.0)) * barWidth;
                float cx1 = barMin.x + static_cast<float>(std::clamp(clip.end / domain, 0.0, 1.0)) * barWidth;
                cx1 = std::max(cx1, cx0 + 2.0f); // keep tiny clips visible
                const float cy0 = laneAreaTop + laneHeight * static_cast<float>(clip.row) + 1.0f;
                const float cy1 = std::max(cy0 + 1.0f, cy0 + laneHeight - 2.0f);
                drawList->AddRectFilled(ImVec2(cx0, cy0), ImVec2(std::min(cx1, barMax.x), cy1),
                                        clipColor, 2.0f);
            }
        }

        const double startFrame = timeline.GetStartTimestamp();
        float x0 = barMin.x + static_cast<float>(std::clamp(startFrame / domain, 0.0, 1.0)) * barWidth;
        float x1 = barMin.x + static_cast<float>(std::clamp((startFrame + visibleFrames) / domain, 0.0, 1.0)) * barWidth;
        // Keep the region rectangle wide enough to see and grab even when zoomed far in.
        const float minRectWidth = 6.0f * uiScale;
        if (x1 - x0 < minRectWidth) {
            const float mid = std::clamp((x0 + x1) * 0.5f,
                                         barMin.x + minRectWidth * 0.5f, barMax.x - minRectWidth * 0.5f);
            x0 = mid - minRectWidth * 0.5f;
            x1 = mid + minRectWidth * 0.5f;
        }
        // Translucent so the clip overview underneath stays visible inside the region.
        drawList->AddRectFilled(ImVec2(x0, barMin.y), ImVec2(x1, barMax.y),
            ImGui::GetColorU32(hovered ? ImGuiCol_ScrollbarGrabHovered : ImGuiCol_ScrollbarGrab, 0.55f), 4.0f);
        drawList->AddRect(ImVec2(x0, barMin.y), ImVec2(x1, barMax.y),
            ImGui::GetColorU32(hovered ? ImGuiCol_ScrollbarGrabHovered : ImGuiCol_ScrollbarGrab), 4.0f);

        if (playheadFrame >= 0.0 && playheadFrame <= domain) {
            const float px = barMin.x + static_cast<float>(playheadFrame / domain) * barWidth;
            drawList->AddLine(ImVec2(px, barMin.y), ImVec2(px, barMax.y),
                              IM_COL32(255, 230, 0, 255), std::max(1.0f, uiScale));
        }

        // Only one ImGui item can be active at a time, so function-local state is safe even
        // though both the Seconds and Beats editors call this helper.
        static bool draggingRegion = false;
        if (ImGui::IsItemActivated())
            draggingRegion = io.MousePos.x >= x0 && io.MousePos.x <= x1;
        if (!ImGui::IsItemActive())
            draggingRegion = false;

        const bool doubleClicked = hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
        if (draggingRegion || doubleClicked) {
            const double frac = std::clamp((io.MousePos.x - barMin.x) / barWidth, 0.0f, 1.0f);
            const double newStart = std::clamp(frac * domain - visibleFrames * 0.5,
                                               0.0, std::max(0.0, domain - visibleFrames));
            timeline.SetStartTimestamp(static_cast<int>(std::llround(newStart)));
        }
    }

    // Vertical wheel over the controller zooms (consumed so nothing else scrolls).
    if (hovered && io.MouseWheel != 0.0f) {
        const float newScale = timeline.GetScale() * std::pow(2.0f, io.MouseWheel * kZoomWheelSensitivity);
        timeline.SetScale(std::clamp(newScale, kMinSafeTimelineScale, kMaxTimelineScale));
        hasExplicitZoom = true;
        io.MouseWheel = 0.0f;
    }

    drawList->AddRect(barMin, barMax, ImGui::GetColorU32(ImGuiCol_Border), 4.0f);

    ImGui::PopID();
}

} // namespace uapmd::gui
