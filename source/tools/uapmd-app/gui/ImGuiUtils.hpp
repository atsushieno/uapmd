#pragma once

#include <imgui.h>

namespace uapmd::gui {

// Returns true *once* when `id` has been held (without significant movement)
// for at least `thresholdSec` seconds.  Call every frame after establishing
// whether the logical area is "hovered".
//
// `isHovered`    – true when the pointer is inside the logical hit area.
// `thresholdSec` – hold duration before the long-press fires (default 0.5 s).
// `moveTolerance` – pointer movement in pixels that cancels the gesture (default 8 px).
inline bool isLongPressedForMobiles(ImGuiID id, bool isHovered,
                          float thresholdSec  = 0.5f,
                          float moveTolerance = 8.0f)
{
    static ImGuiID  activeId    = 0;
    static double   pressStart  = 0.0;
    static ImVec2   pressPos    = {0.f, 0.f};

    const double  now      = ImGui::GetTime();
    const ImVec2  mousePos = ImGui::GetMousePos();

    // Arm on first press over this item.
    if (isHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        activeId   = id;
        pressStart = now;
        pressPos   = mousePos;
    }

    if (activeId != id)
        return false;

    // Cancel if the pointer moved too far.
    const float dx = mousePos.x - pressPos.x;
    const float dy = mousePos.y - pressPos.y;
    if (dx * dx + dy * dy > moveTolerance * moveTolerance) {
        activeId = 0;
        return false;
    }

    // Cancel if the button was released before the threshold.
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        activeId = 0;
        return false;
    }

    // Fire once when the threshold is reached.
    if (now - pressStart >= static_cast<double>(thresholdSec)) {
        activeId = 0;
        return true;
    }

    return false;
}

// Convenience wrapper for standard ImGui widgets: call immediately after the
// widget (e.g. Selectable, Button) to test the last rendered item.
// Intended for mobile platforms (Android / iOS) where double-click is not
// practical; on desktop the double-click handler takes precedence.
inline bool isItemLongPressedForMobiles(float thresholdSec  = 0.5f,
                                        float moveTolerance = 8.0f)
{
    return isLongPressedForMobiles(ImGui::GetItemID(), ImGui::IsItemHovered(),
                         thresholdSec, moveTolerance);
}

} // namespace uapmd::gui
