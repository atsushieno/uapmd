#pragma once

namespace ImTimeline {
    class Timeline;
}

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

// Multiplier applied per unit of io.MouseWheel when scrolling over the position controller:
// each wheel "tick" changes the zoom by 2^kZoomWheelSensitivity (e.g. 0.2 -> ~15% per tick).
constexpr float kZoomWheelSensitivity = 0.2f;

// Height of the navigation row (zoom slider + position controller) above the track list.
constexpr float kNavigatorHeightPt = 50.0f;

// Renders the navigation row above the timeline:
//
//   | zoom slider (legend width) | position controller (content width) |
//
// The position controller shows the currently visible region as a rectangle within the whole
// song (contentFrames long, in this timeline's units -- seconds, or display ticks). Dragging a
// press that started inside the rectangle positions the visible region absolutely (its center
// follows the pointer, per spec: destination is the location on the controller, not a delta);
// double-clicking anywhere on the controller jumps the same way; a single click outside the
// rectangle does nothing (stray taps are common on touch). Vertical mouse wheel over the
// controller zooms. Pass playheadFrame < 0 to hide the playhead marker.
//
// barStartScreenX anchors the controller's left edge absolutely (screen space) so it aligns
// with the track content column below regardless of what else precedes it on the toolbar row;
// the zoom slider fills whatever remains between the current cursor and that boundary.
void renderTimelineNavigator(ImTimeline::Timeline& timeline, bool& hasExplicitZoom,
                             float uiScale, float barStartScreenX,
                             double contentFrames, double playheadFrame,
                             float visibleWidthPixels);

} // namespace uapmd::gui
