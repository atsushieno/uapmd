#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "TimelineAxis.hpp"

namespace ImTimeline {
    class Timeline;
}

namespace uapmd_app_gui {

// One clip in the navigator's whole-song overview: which track lane it sits on (0-based, top
// to bottom, in the same order the timeline lists its sections) and its extent in the active
// axis's frames.
struct NavigatorClip {
    int row{0};
    double start{0.0};
    double end{0.0};
};

struct NavigatorRenderProgress {
    int32_t trackNumber{0};
    double progress{0.0};
    double renderedSeconds{0.0};
    double totalSeconds{0.0};
};

// Multiplier applied per unit of io.MouseWheel when scrolling over the position controller:
// each wheel "tick" changes the zoom by 2^kZoomWheelSensitivity (e.g. 0.2 -> ~15% per tick).
constexpr float kZoomWheelSensitivity = 0.2f;

// Height of the navigation row (zoom slider + position controller) above the track list.
constexpr float kNavigatorHeightPt = 40.0f;

// Renders the navigation row above the timeline:
//
//   | zoom slider (legend width) | position controller (content width) |
//
// The position controller shows the currently visible region as a rectangle within the whole
// song (contentFrames long, in the axis's frames). Dragging a press that started inside the
// rectangle positions the visible region absolutely (its center
// follows the pointer, per spec: destination is the location on the controller, not a delta);
// double-clicking anywhere on the controller jumps the same way; a single click outside the
// rectangle does nothing (stray taps are common on touch). Vertical mouse wheel over the
// controller zooms. Pass playheadFrame < 0 to hide the playhead marker.
//
// barStartScreenX anchors the controller's left edge absolutely (screen space) so it aligns
// with the track content column below regardless of what else precedes it on the toolbar row;
// the zoom slider fills whatever remains between the current cursor and that boundary.
//
// The controller doubles as a whole-song overview: `clips` are drawn as per-track bars
// (rowCount lanes stacked top to bottom) underneath the translucent visible-region rectangle.
// The zoom slider reads and writes pixels per *unit* (per second, or per beat) rather than the
// pixels-per-frame ImTimeline stores, so the number shown means the same thing on either axis --
// the axis supplies both the conversion and the bounds.
void renderTimelineNavigator(ImTimeline::Timeline& timeline, bool& hasExplicitZoom,
                             const TimelineAxis& axis,
                             float uiScale, float barStartScreenX,
                             double contentFrames, double playheadFrame,
                             float visibleWidthPixels,
                             const std::vector<NavigatorClip>& clips, int rowCount,
                             const std::optional<NavigatorRenderProgress>& renderProgress);

} // namespace uapmd_app_gui
