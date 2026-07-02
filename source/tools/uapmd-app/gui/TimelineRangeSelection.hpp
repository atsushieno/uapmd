#pragma once

#include <algorithm>
#include <cstdint>

namespace uapmd::gui {

// Tracks a drag-to-select-a-range gesture within a single track's clip lane. Unit-agnostic
// (frame ints, same convention as TimelineLaneAssignment.hpp) -- the caller supplies frame
// coordinates already converted from pixels, and converts the finished range back to seconds
// itself (seconds-frames for SequenceEditor, beat-ticks for BeatsSequenceEditor).
struct RangeSelectionDrag {
    bool active = false;
    int32_t trackIndex = -1;
    int32_t anchorFrame = 0;   // frame under the mouse at mouse-down
    int32_t currentFrame = 0;  // frame under the mouse this frame
};

// Call once per frame while the left mouse button is down. A candidate drag only starts on a
// fresh click (`mouseClicked`) that isn't over an existing clip node -- clicks on a node are
// ImTimeline's own node-drag, not a range selection -- and that landed inside a track's lane
// (`hoveredTrackIndex != -1`). Once started, the drag stays locked to that track regardless of
// where the mouse moves afterward (matches standard DAW range-select behavior).
inline void updateRangeSelectionDrag(
    RangeSelectionDrag& drag,
    bool mouseDown,
    bool mouseClicked,
    bool overNodeAtClick,
    int32_t hoveredTrackIndex,
    int32_t frameUnderMouse
) {
    if (mouseClicked && !overNodeAtClick && hoveredTrackIndex != -1) {
        drag.active = true;
        drag.trackIndex = hoveredTrackIndex;
        drag.anchorFrame = frameUnderMouse;
        drag.currentFrame = frameUnderMouse;
    } else if (drag.active && mouseDown) {
        drag.currentFrame = frameUnderMouse;
    }
}

// Call on mouse release. Returns true (and fills the out-params, start <= end) when the drag
// moved far enough to count as a real range rather than a plain click; resets `drag` either way.
inline bool finishRangeSelectionDrag(
    RangeSelectionDrag& drag,
    float pixelsDragged,
    float dragThresholdPx,
    int32_t& outTrackIndex,
    int32_t& outStartFrame,
    int32_t& outEndFrame
) {
    const bool isRange = drag.active && pixelsDragged >= dragThresholdPx;
    if (isRange) {
        outTrackIndex = drag.trackIndex;
        outStartFrame = std::min(drag.anchorFrame, drag.currentFrame);
        outEndFrame = std::max(drag.anchorFrame, drag.currentFrame);
    }
    drag.active = false;
    return isRange;
}

} // namespace uapmd::gui
