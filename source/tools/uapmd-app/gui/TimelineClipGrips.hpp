#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace uapmd_app_gui {

// The handles a timeline clip is edited through.
//
// A clip's body only selects it; moving and resizing go through a grip at each end, so a stray
// drag across the lanes can no longer carry a clip off. Each grip is split in two: its upper half
// resizes that end, its lower half moves the clip. A grip sits outside its clip, appended to that
// end, so the content stays unobscured. The same layout as uapmd-cmp's timeline.
enum class ClipEdge { Start, End };
enum class GripAction { Move, ResizeStart, ResizeEnd };

// One clip as the grip layout sees it: which lane it is in, and its horizontal extent in pixels.
struct ClipSpan {
    int32_t trackIndex;
    int32_t clipId;
    float laneTop;    // lanes are told apart by their top edge within the track
    float left;
    float right;
};

struct ClipGrip {
    int32_t trackIndex;
    int32_t clipId;
    float laneTop;
    ClipEdge edge;
    float left;
    float right;
    float edgeX;      // the clip edge the grip belongs to; decides between overlapping grips
};

// Where every grip goes. A grip is `width` wide and appended to its end of the clip. Where two
// clips in a lane are closer than two grips, both are placed around the middle of the gap
// instead -- each on its own side of it, reaching into its own clip once the gap is narrower
// still -- so butted clips get their grips just inside their own edges, and two grips never
// share a pixel. A clip too close to the lane's start for its start grip gets it inside itself.
inline std::vector<ClipGrip> layoutClipGrips(const std::vector<ClipSpan>& clips, float width) {
    std::map<std::pair<int32_t, float>, std::vector<ClipSpan>> lanes;
    for (const auto& clip : clips)
        lanes[{clip.trackIndex, clip.laneTop}].push_back(clip);

    std::vector<ClipGrip> grips;
    grips.reserve(clips.size() * 2);
    for (auto& [lane, inLane] : lanes) {
        std::sort(inLane.begin(), inLane.end(), [](const ClipSpan& a, const ClipSpan& b) {
            return a.left != b.left ? a.left < b.left : a.clipId < b.clipId;
        });
        for (size_t i = 0; i < inLane.size(); ++i) {
            const auto& clip = inLane[i];
            const ClipSpan* previous = i > 0 ? &inLane[i - 1] : nullptr;
            float startLeft;
            if (previous && clip.left - previous->right < width * 2.0f)
                startLeft = (previous->right + clip.left) / 2.0f;
            else if (!previous && clip.left < width)
                startLeft = clip.left;
            else
                startLeft = clip.left - width;
            grips.push_back({clip.trackIndex, clip.clipId, clip.laneTop, ClipEdge::Start,
                             startLeft, startLeft + width, clip.left});

            const ClipSpan* next = i + 1 < inLane.size() ? &inLane[i + 1] : nullptr;
            const float endRight = next && next->left - clip.right < width * 2.0f
                ? (clip.right + next->left) / 2.0f
                : clip.right + width;
            grips.push_back({clip.trackIndex, clip.clipId, clip.laneTop, ClipEdge::End,
                             endRight - width, endRight, clip.right});
        }
    }
    return grips;
}

// The grip under x in the lane at (trackIndex, laneTop), if any. A clip narrower than its two
// grips has them overlap; the one whose edge is nearer the pointer wins.
inline const ClipGrip* gripAt(const std::vector<ClipGrip>& grips, int32_t trackIndex, float laneTop, float x) {
    const ClipGrip* best = nullptr;
    for (const auto& grip : grips) {
        if (grip.trackIndex != trackIndex || grip.laneTop != laneTop || x < grip.left || x > grip.right)
            continue;
        if (!best || std::abs(grip.edgeX - x) < std::abs(best->edgeX - x))
            best = &grip;
    }
    return best;
}

inline GripAction gripAction(ClipEdge edge, bool upperHalf) {
    if (!upperHalf)
        return GripAction::Move;
    return edge == ClipEdge::Start ? GripAction::ResizeStart : GripAction::ResizeEnd;
}

struct ClipExtentSeconds {
    double start;
    double end;
    bool operator==(const ClipExtentSeconds&) const = default;
};

// Where a grip drag would put the clip, given where the pointer has taken the dragged edge (the
// start, for a move) -- the same answer for the overlay drawn during the drag and for the edit
// committed on release. `snap` moves that edge onto the Snap grid; without it the edge goes
// where the pointer is. A start resize stops at earliestStart (the timeline's start for a MIDI
// clip, whose content shifts along; its source's start for an audio clip), and no clip becomes
// shorter than minLength.
inline ClipExtentSeconds proposeClipEdit(GripAction action,
                                         ClipExtentSeconds current,
                                         double earliestStart,
                                         double draggedEdgeSeconds,
                                         const std::function<double(double)>& snap,
                                         double minLength) {
    const double edge = snap ? snap(draggedEdgeSeconds) : draggedEdgeSeconds;
    switch (action) {
        case GripAction::Move: {
            const double start = std::max(0.0, edge);
            return {start, current.end + (start - current.start)};
        }
        case GripAction::ResizeEnd:
            return {current.start, std::max(edge, current.start + minLength)};
        case GripAction::ResizeStart: {
            const double latest = current.end - minLength;
            return {std::clamp(edge, std::min(std::max(0.0, earliestStart), latest), latest), current.end};
        }
    }
    return current;
}

} // namespace uapmd_app_gui
