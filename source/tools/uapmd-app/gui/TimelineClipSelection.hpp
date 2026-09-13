#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <vector>
#include <imgui.h>
#include <imgui_internal.h>

namespace uapmd_app_gui {

struct TimelineClipTarget {
    int32_t track_index;
    int32_t clip_id;
};

// Selection is owned by TimelineEditor, independently of ImTimeline node identities.
struct TimelineClipActions {
    std::function<bool(int32_t, int32_t)> isSelected;
    std::function<void(const std::vector<TimelineClipTarget>&, bool, bool)> select;
    std::function<void(int32_t, int32_t, double)> renderMenu;
};

struct TimelineClipHitBox {
    TimelineClipTarget target;
    ImVec2 min;
    ImVec2 max;
    ImVec2 visual_min;
    ImVec2 visual_max;
    float border_radius = 0.0f;
    float border_thickness = 1.0f;
};

// DrawTimeline creates a child window for clip content. Drawing overlays on
// its parent puts them behind that child's clips, regardless of call order.
// Called immediately after DrawTimeline, before rendering any editor popups.
inline ImDrawList* timelineSelectionDrawList() {
    auto* window = ImGui::GetCurrentWindow();
    return window->DC.ChildWindows.empty()
        ? window->DrawList : window->DC.ChildWindows.back()->DrawList;
}

struct TimelineClipMarquee {
    bool active = false;
    bool additive = false;
    ImVec2 anchor;

    void render(const TimelineClipActions& actions,
                const std::vector<TimelineClipHitBox>& boxes,
                ImVec2 areaMin, ImVec2 areaMax, bool acceptsInput, float uiScale) {
        if (!actions.select || !actions.isSelected)
            return;
        const auto mouse = ImGui::GetMousePos();
        const bool inArea = mouse.x >= areaMin.x && mouse.x <= areaMax.x &&
            mouse.y >= areaMin.y && mouse.y <= areaMax.y;
        const auto hit = std::find_if(boxes.begin(), boxes.end(), [&](const auto& box) {
            return mouse.x >= box.min.x && mouse.x <= box.max.x &&
                mouse.y >= box.min.y && mouse.y <= box.max.y;
        });
        const auto& io = ImGui::GetIO();
        if (acceptsInput && inArea && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyAlt) {
            if (hit != boxes.end()) {
                const bool toggle = io.KeyCtrl || io.KeySuper;
                if (toggle || io.KeyShift || !actions.isSelected(hit->target.track_index, hit->target.clip_id))
                    actions.select({hit->target}, io.KeyShift || toggle, toggle);
            } else {
                active = true;
                additive = io.KeyShift;
                anchor = mouse;
            }
        }
        auto* draw = timelineSelectionDrawList();
        draw->PushClipRect(areaMin, areaMax, true);
        for (const auto& box : boxes)
            if (actions.isSelected(box.target.track_index, box.target.clip_id)) {
                // Match ClipPreview's border inset and rounded corners.
                const ImVec2 borderMax(box.visual_max.x - box.border_thickness,
                                       box.visual_max.y - box.border_thickness);
                draw->AddRectFilled(box.visual_min, borderMax, ImGui::GetColorU32(ImGuiCol_Header, 0.35f), box.border_radius);
                draw->AddRect(box.visual_min, borderMax, ImGui::GetColorU32(ImGuiCol_HeaderActive),
                              box.border_radius, 0, box.border_thickness);
            }
        if (active) {
            const ImVec2 end(std::clamp(mouse.x, areaMin.x, areaMax.x),
                             std::clamp(mouse.y, areaMin.y, areaMax.y));
            const ImVec2 min(std::min(anchor.x, end.x), std::min(anchor.y, end.y));
            const ImVec2 max(std::max(anchor.x, end.x), std::max(anchor.y, end.y));
            draw->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_Header, 0.3f));
            draw->AddRect(min, max, ImGui::GetColorU32(ImGuiCol_HeaderActive));
            if (ImGui::IsKeyPressed(ImGuiKey_Escape))
                active = false;
            else if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                std::vector<TimelineClipTarget> targets;
                if (max.x - min.x >= 4.0f * uiScale || max.y - min.y >= 4.0f * uiScale)
                    for (const auto& box : boxes)
                        if (box.max.x > min.x && box.min.x < max.x &&
                                box.max.y > min.y && box.min.y < max.y)
                            targets.push_back(box.target);
                actions.select(targets, additive, false);
                active = false;
            }
        } else if (acceptsInput && ImGui::IsKeyPressed(ImGuiKey_Escape))
            actions.select({}, false, false);
        draw->PopClipRect();
    }
};

}
