#pragma once

#include <cstdint>

#include <imgui.h>

#include <uapmd-data/uapmd-data.hpp>

namespace uapmd_app_gui {

// Which ruler the unified timeline is showing. This is the only thing a "view mode" changes:
// the timeline widget, its clips, and every gesture on them stay the same object either way.
enum class TimelineAxisMode { Seconds, Beats };

// Maps the model's real-world seconds onto ImTimeline's int32 frame axis, and draws the ruler
// and grid that give those frames a meaning a reader can use.
//
// The frame unit is deliberately far finer than anything the ruler labels. ImTimeline stores
// node start/end as int32 frames, which makes the frame unit the finest position a clip can
// hold while it is on screen -- and a drag writes that position straight back to the model. A
// coarse frame unit therefore does not merely round what you see, it rounds what you keep.
class TimelineAxis {
public:
    // A frame is one millisecond on the seconds axis, and 1/kFramesPerBeat of a quarter-note
    // beat on the beats axis. Both leave int32 room for far longer projects than are playable:
    // about 24 days, and about 22 million beats.
    static constexpr double kFramesPerSecond = 1000.0;
    static constexpr double kFramesPerBeat = 96.0;

    // Zoom bounds in pixels per *unit* -- per second, or per beat -- rather than per frame, so
    // the usable zoom range is identical on both axes and does not shift if a frame unit above
    // is ever retuned.
    static constexpr float kMinScalePerUnit = 0.05f;
    static constexpr float kMaxScalePerUnit = 400.0f;
    static constexpr float kDefaultScalePerUnit = 5.0f;
    static constexpr float kDefaultBeatsScalePerUnit = 24.0f;

    void setMode(TimelineAxisMode mode) { mode_ = mode; }
    TimelineAxisMode mode() const { return mode_; }
    bool isBeats() const { return mode_ == TimelineAxisMode::Beats; }

    // The tempo map is owned by TimelineEditor and outlives the axis's use of it. A null one
    // leaves the beats axis on the same 120 BPM an empty map converts at.
    void setTempoMap(const uapmd::TempoMap* tempoMap) { tempoMap_ = tempoMap; }

    const char* unitsLabel() const { return isBeats() ? "beats" : "seconds"; }
    double framesPerUnit() const { return isBeats() ? kFramesPerBeat : kFramesPerSecond; }

    // seconds <-> display unit (one second, or one quarter-note beat)
    double unitsFromSeconds(double seconds) const;
    double secondsFromUnits(double units) const;

    // unit <-> frame. frameFromUnits clamps into int32 and never returns a negative frame;
    // it is the half callers reach for when they already hold a length in units (a clip whose
    // width is authored in beats, say) rather than in seconds.
    int32_t frameFromUnits(double units) const;
    double unitsFromFrame(double frame) const { return frame / framesPerUnit(); }

    // seconds <-> frame, i.e. the above composed with the tempo conversion.
    int32_t frameFromSeconds(double seconds) const { return frameFromUnits(unitsFromSeconds(seconds)); }
    double secondsFromFrame(double frame) const { return secondsFromUnits(unitsFromFrame(frame)); }

    // All in pixels per frame, which is what ImTimeline::SetScale takes.
    float defaultScale(float uiScale) const;
    float minScale() const { return kMinScalePerUnit / static_cast<float>(framesPerUnit()); }
    float maxScale() const { return kMaxScalePerUnit / static_cast<float>(framesPerUnit()); }

    // How far the timeline should extend past the last clip, so there is somewhere to drop a
    // new one: a few seconds, or a few bars.
    int32_t trailingPadFrames() const;

    // Geometry of one timeline render, in screen pixels.
    struct RulerGeometry {
        float headerMinY{0.0f};   // top of the ruler strip
        float headerMaxY{0.0f};   // bottom of the ruler strip, i.e. top of the clip lanes
        float contentMinX{0.0f};  // left edge of the clip area, i.e. right of the legend
        float contentMaxX{0.0f};
        float contentMaxY{0.0f};  // bottom of the clip lanes
        double startFrame{0.0};
        float scale{0.0f};        // pixels per frame
        float uiScale{1.0f};
        // The ruler strip and the clip lanes belong to different ImGui windows, and ImGui renders
        // a child's draw list after its parent's. Grid lines issued to the parent's list would
        // therefore be painted underneath the opaque section fills, so the caller hands over the
        // list that renders on top of them; the header strip is not covered by anything and uses
        // the ordinary window list.
        ImDrawList* headerDrawList{nullptr};
        ImDrawList* gridDrawList{nullptr};
    };

    // Draws the ruler ticks and labels across the header strip, and the matching grid lines down
    // the clip lanes. ImTimeline's own header numbering is suppressed by the caller because it
    // can only print the raw frame index -- a millisecond, or a 96th of a beat -- which names
    // nothing a reader is looking for.
    void drawRuler(const RulerGeometry& geometry) const;

private:
    void drawSecondsRuler(const RulerGeometry& geometry) const;
    void drawBeatsRuler(const RulerGeometry& geometry) const;

    TimelineAxisMode mode_{TimelineAxisMode::Seconds};
    const uapmd::TempoMap* tempoMap_{nullptr};
};

} // namespace uapmd_app_gui
