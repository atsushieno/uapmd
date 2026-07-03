#pragma once

#include <cmath>

namespace uapmd::gui {

// ImTimeline's HorizontalNodeView truncates GetScale() to s32 before use, so any scale below 1.0
// collapses every clip to zero width. kMinSafeTimelineScale keeps a safety margin above that hard
// floor; kMaxTimelineScale is just a sane upper bound against absurd over-zoom. Shared by both
// SequenceEditor and BeatsSequenceEditor since the underlying constraint is the same regardless
// of what a "frame" represents (a second, or a kTicksPerBeatDisplay-th of a beat).
constexpr float kMinSafeTimelineScale = 1.5f;
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

} // namespace uapmd::gui
