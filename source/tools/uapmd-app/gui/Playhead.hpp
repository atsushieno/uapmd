#pragma once

#include <algorithm>
#include <imgui.h>

namespace uapmd_app_gui {

inline constexpr ImU32 kPlayheadColor = IM_COL32(255, 230, 0, 255);

inline void drawPlayheadBar(ImDrawList* drawList, float x, ImVec2 min, ImVec2 max, float uiScale) {
    if (!(x >= min.x && x <= max.x && max.x > min.x && max.y > min.y))
        return;

    drawList->PushClipRect(min, max, true);
    drawList->AddLine({x, min.y}, {x, max.y}, kPlayheadColor, std::max(1.0f, 2.0f * uiScale));
    drawList->PopClipRect();
}

} // namespace uapmd_app_gui
