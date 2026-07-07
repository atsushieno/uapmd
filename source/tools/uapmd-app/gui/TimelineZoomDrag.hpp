#pragma once

#include <cmath>

#include <imgui.h>
#include <ImTimeline.h>

namespace uapmd::gui {

// ImTimeline's HorizontalNodeView used to truncate GetScale() to s32 before use, so any scale
// below 1.0 collapsed every clip to zero width -- that's now patched (see imtimeline.patch) to use
// the float scale directly, so this floor only needs to stay clear of literal zero/degenerate
// values, not the old truncation cliff. kMaxTimelineScale is just a sane upper bound against absurd
// over-zoom. Shared by both SequenceEditor and BeatsSequenceEditor since the underlying constraint
// is the same regardless of what a "frame" represents (a second, or a kTicksPerBeatDisplay-th of a
// beat) -- a low floor here is what lets a very long song be zoomed out far enough to fit on screen.
constexpr float kMinSafeTimelineScale = 0.05f;
constexpr float kMaxTimelineScale = 400.0f;

// Multiplier applied per unit of io.MouseWheel when scrolling over a timeline's header: each
// wheel "tick" changes the zoom by 2^kZoomWheelSensitivity (e.g. 0.2 -> ~15% per tick).
constexpr float kZoomWheelSensitivity = 0.2f;

// Trackpad two-finger scrolling reports both axes at once even for a gesture the user intends
// as purely horizontal -- a mostly-horizontal swipe still carries a small residual vertical
// component. Require the vertical delta to clear this deadzone *and* dominate the horizontal
// delta before treating it as a zoom gesture; otherwise leave it alone so ImTimeline's own
// io.MouseWheelH-driven horizontal pan behaves as expected.
constexpr float kZoomWheelDeadzone = 0.3f;

// True if this frame's vertical wheel delta should be treated as an intentional zoom gesture
// rather than incidental noise from a horizontal scroll.
inline bool isIntentionalZoomWheel(float mouseWheel, float mouseWheelH) {
    return std::abs(mouseWheel) > kZoomWheelDeadzone && std::abs(mouseWheel) > std::abs(mouseWheelH);
}

// Press-and-hold over the header before the zoom popup opens. Touch devices have no mouse
// wheel, so this is the touch path to the same zoom control (SDL synthesizes mouse events
// from single-finger touch, so no backend-level gesture handling is needed).
constexpr float kZoomHoldSeconds = 0.5f;
// Finger drift allowed during the hold (in unscaled pixels) before it stops counting as a
// stationary press -- generous, since fingers wobble far more than mouse cursors.
constexpr float kZoomHoldMaxDriftPx = 12.0f;

// Call once per frame from inside the timeline's child window (so the popup shares its ID
// scope). Opens a slider popup when the pointer is held still over the header for
// kZoomHoldSeconds; the slider edits the timeline zoom live. ImGui closes the popup on any
// click/touch outside it. Returns true while the popup is open.
inline bool updateZoomHoldPopup(const char* popupId, bool overHeader,
                                ImTimeline::Timeline& timeline, bool& hasExplicitZoom,
                                float uiScale) {
    ImGuiIO& io = ImGui::GetIO();
    const float maxDrift = kZoomHoldMaxDriftPx * uiScale;
    if (overHeader && io.MouseDown[0] &&
        io.MouseDownDuration[0] >= kZoomHoldSeconds &&
        io.MouseDragMaxDistanceSqr[0] <= maxDrift * maxDrift &&
        !ImGui::IsPopupOpen(popupId))
        ImGui::OpenPopup(popupId);

    bool open = false;
    if (ImGui::BeginPopup(popupId)) {
        open = true;
        float scale = timeline.GetScale();
        ImGui::TextUnformatted("Zoom");
        ImGui::SetNextItemWidth(220.0f * uiScale);
        if (ImGui::SliderFloat("##TimelineZoom", &scale,
                               kMinSafeTimelineScale, kMaxTimelineScale, "%.2f",
                               ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp)) {
            timeline.SetScale(scale);
            hasExplicitZoom = true;
        }
        ImGui::EndPopup();
    }
    return open;
}

} // namespace uapmd::gui
