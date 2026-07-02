#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace uapmd::gui {

// 1 ImTimeline "frame" on the beats timeline represents 1/kTicksPerBeatDisplay of a
// quarter-note beat. This gives sub-beat placement precision while keeping node start/end
// within ImTimeline's plain int32_t frame axis.
//
// Kept deliberately small (a 16th-note-in-4/4 subdivision): ImTimeline's own node-width
// rendering (HorizontalNodeView::DrawNodeView) truncates GetScale() to an integer pixels-per-
// frame value before multiplying by node.start/end, so once the effective zoom drops below
// 1 pixel per *frame* (not per beat) all clip boxes silently collapse to zero width. A large
// display resolution (e.g. 960, MIDI-file-style PPQ) would force that breakage at an unusably
// small zoomed-out range (only a couple of bars visible); 4 keeps a full song comfortably
// zoomable out while still placing clips well inside single-beat precision.
constexpr int32_t kTicksPerBeatDisplay = 4;

// Converts a quarter-note-beat position/length to a display frame, analogous to the seconds
// editor's toTimelineFrame().
inline int32_t toBeatFrame(double beats) {
    if (!std::isfinite(beats))
        return 0;
    const double maxUnits = static_cast<double>(std::numeric_limits<int32_t>::max() - 1) / kTicksPerBeatDisplay;
    const double clamped = std::clamp(beats, 0.0, maxUnits);
    return static_cast<int32_t>(std::llround(clamped * kTicksPerBeatDisplay));
}

} // namespace uapmd::gui
