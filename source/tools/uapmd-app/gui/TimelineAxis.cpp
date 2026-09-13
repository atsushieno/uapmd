#include "TimelineAxis.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <string>
#include <vector>

namespace uapmd_app_gui {

namespace {

constexpr double kFallbackBpm = 120.0;

// Upper bound on lines drawn per ruler pass. Every step choice below already targets a minimum
// pixel spacing, so this only ever trips on a degenerate scale, where drawing nothing beats
// hanging the frame.
constexpr int kMaxRulerLines = 4000;

ImVec4 withAlpha(const ImVec4& color, float alpha) {
    return ImVec4(color.x, color.y, color.z, alpha);
}

// Smallest "round" number of seconds at least as large as `minimum`, taken from a ladder that
// stays readable as labels (no 2.5s or 7s gradations) and lines up with clock time above a
// minute.
double niceSecondsStep(double minimum) {
    static constexpr double kLadder[] = {
        0.001, 0.002, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.5,
        1.0, 2.0, 5.0, 10.0, 15.0, 30.0,
        60.0, 120.0, 300.0, 600.0, 900.0, 1800.0, 3600.0,
    };
    for (const double candidate : kLadder)
        if (candidate >= minimum)
            return candidate;
    // Past an hour, keep doubling rather than inventing more ladder entries.
    double step = kLadder[std::size(kLadder) - 1];
    while (step < minimum && step < 1e9)
        step *= 2.0;
    return step;
}

std::string formatSecondsLabel(double seconds, double step) {
    if (seconds < 0.0)
        seconds = 0.0;
    if (step >= 1.0) {
        const auto total = static_cast<long long>(std::llround(seconds));
        if (total >= 60)
            return std::format("{}:{:02}", total / 60, total % 60);
        return std::format("{}s", total);
    }
    // Show exactly as many decimals as the step needs, so neighbouring labels differ.
    const int decimals = std::clamp(static_cast<int>(std::ceil(-std::log10(step))), 1, 3);
    return std::format("{:.{}f}s", seconds, decimals);
}

} // namespace

double TimelineAxis::unitsFromSeconds(double seconds) const {
    if (!isBeats())
        return seconds;
    if (tempoMap_)
        return tempoMap_->secondsToBeats(seconds);
    return std::max(0.0, seconds) * (kFallbackBpm / 60.0);
}

double TimelineAxis::secondsFromUnits(double units) const {
    if (!isBeats())
        return units;
    if (tempoMap_)
        return tempoMap_->beatsToSeconds(units);
    return std::max(0.0, units) * (60.0 / kFallbackBpm);
}

int32_t TimelineAxis::frameFromUnits(double units) const {
    if (!std::isfinite(units))
        return 0;
    const double maxFrames = static_cast<double>(std::numeric_limits<int32_t>::max() - 1);
    const double frames = std::clamp(units * framesPerUnit(), 0.0, maxFrames);
    return static_cast<int32_t>(std::llround(frames));
}

float TimelineAxis::defaultScale(float uiScale) const {
    const float perUnit = (isBeats() ? kDefaultBeatsScalePerUnit : kDefaultScalePerUnit) * uiScale;
    const float perFrame = perUnit / static_cast<float>(framesPerUnit());
    return std::clamp(perFrame, minScale(), maxScale());
}

int32_t TimelineAxis::trailingPadFrames() const {
    // Four bars of 4/4, or four seconds -- enough room to drop a clip past the last one without
    // the timeline visibly jumping when it does.
    return static_cast<int32_t>((isBeats() ? 16.0 : 4.0) * framesPerUnit());
}

void TimelineAxis::drawRuler(const RulerGeometry& geometry) const {
    if (geometry.scale <= 0.0f || geometry.contentMaxX <= geometry.contentMinX)
        return;
    if (isBeats())
        drawBeatsRuler(geometry);
    else
        drawSecondsRuler(geometry);
}

void TimelineAxis::drawSecondsRuler(const RulerGeometry& g) const {
    const double pixelsPerSecond = static_cast<double>(g.scale) * kFramesPerSecond;
    if (pixelsPerSecond <= 0.0)
        return;

    const double startSeconds = g.startFrame / kFramesPerSecond;
    const double endSeconds = startSeconds +
        static_cast<double>(g.contentMaxX - g.contentMinX) / pixelsPerSecond;

    // A labelled tick every ~80 dp keeps the text from colliding at any zoom.
    const double majorStep = niceSecondsStep((80.0 * g.uiScale) / pixelsPerSecond);
    // Subdivide only when the minor lines would be far enough apart to read as separate.
    const double minorStep = (majorStep / 5.0) * pixelsPerSecond >= 6.0 ? majorStep / 5.0 : majorStep;

    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec4 text = style.Colors[ImGuiCol_Text];
    const ImVec4 textDisabled = style.Colors[ImGuiCol_TextDisabled];
    const ImU32 majorLine = ImGui::GetColorU32(withAlpha(text, 0.30f));
    const ImU32 minorLine = ImGui::GetColorU32(withAlpha(text, 0.10f));
    const ImU32 tickColor = ImGui::GetColorU32(withAlpha(textDisabled, 1.0f));

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->PushClipRect(ImVec2(g.contentMinX, g.headerMinY),
                           ImVec2(g.contentMaxX, g.contentMaxY), true);

    const float headerHeight = std::max(0.0f, g.headerMaxY - g.headerMinY);
    long long index = static_cast<long long>(std::floor(startSeconds / minorStep));
    for (int drawn = 0; drawn < kMaxRulerLines; ++drawn, ++index) {
        const double seconds = static_cast<double>(index) * minorStep;
        if (seconds > endSeconds + minorStep)
            break;
        if (seconds < 0.0)
            continue;

        const float x = g.contentMinX +
            static_cast<float>((seconds - startSeconds) * pixelsPerSecond);
        if (x < g.contentMinX || x > g.contentMaxX)
            continue;

        // Compare in step units rather than seconds: majorStep is often not representable in
        // binary, so `fmod(seconds, majorStep)` drifts into false negatives as index grows.
        const auto perMajor = static_cast<long long>(std::llround(majorStep / minorStep));
        const bool isMajor = perMajor <= 1 || (index % perMajor) == 0;

        drawList->AddLine(ImVec2(x, g.headerMaxY), ImVec2(x, g.contentMaxY),
                          isMajor ? majorLine : minorLine,
                          isMajor ? 1.5f * g.uiScale : 1.0f * g.uiScale);

        if (headerHeight <= 0.0f)
            continue;
        const float tickTop = g.headerMaxY - headerHeight * (isMajor ? 0.5f : 0.25f);
        drawList->AddLine(ImVec2(x, tickTop), ImVec2(x, g.headerMaxY), tickColor, 1.0f);
        if (isMajor)
            drawList->AddText(ImVec2(x + 3.0f * g.uiScale, g.headerMinY), tickColor,
                              formatSecondsLabel(seconds, majorStep).c_str());
    }

    drawList->PopClipRect();
}

void TimelineAxis::drawBeatsRuler(const RulerGeometry& g) const {
    const double pixelsPerBeat = static_cast<double>(g.scale) * kFramesPerBeat;
    if (pixelsPerBeat <= 0.0)
        return;

    const double startBeat = g.startFrame / kFramesPerBeat;
    const double endBeat = startBeat +
        static_cast<double>(g.contentMaxX - g.contentMinX) / pixelsPerBeat;

    // Signature regions to walk: the tempo map's effective signatures, or a single implicit 4/4
    // region when no time-signature meta events exist yet.
    std::vector<uapmd::TempoMap::EffectiveSignature> regions;
    if (tempoMap_)
        regions = tempoMap_->effectiveSignatures();
    if (regions.empty() || regions.front().startBeat > 1e-9) {
        uapmd::TempoMap::EffectiveSignature defaultRegion;
        defaultRegion.startBeat = 0.0;
        defaultRegion.endBeat = regions.empty()
            ? std::numeric_limits<double>::infinity() : regions.front().startBeat;
        defaultRegion.numerator = 4;
        defaultRegion.denominator = 4;
        regions.insert(regions.begin(), defaultRegion);
    }

    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec4 text = style.Colors[ImGuiCol_Text];
    const ImVec4 textDisabled = style.Colors[ImGuiCol_TextDisabled];
    const ImU32 barColor = ImGui::GetColorU32(withAlpha(text, 0.35f));
    const ImU32 beatColor = ImGui::GetColorU32(withAlpha(text, 0.12f));
    const ImU32 tickColor = ImGui::GetColorU32(withAlpha(textDisabled, 1.0f));
    const float barThickness = 1.5f * g.uiScale;
    const float beatThickness = 1.0f * g.uiScale;
    const float headerHeight = std::max(0.0f, g.headerMaxY - g.headerMinY);

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->PushClipRect(ImVec2(g.contentMinX, g.headerMinY),
                           ImVec2(g.contentMaxX, g.contentMaxY), true);

    // Bar numbers are continuous across signature changes, so every region has to know how many
    // bars the regions before it contributed.
    long long barsBefore = 0;
    int drawn = 0;
    for (const auto& region : regions) {
        const uint8_t numerator = region.numerator > 0 ? region.numerator : 4;
        const uint8_t denominator = region.denominator > 0 ? region.denominator : 4;
        // One "signature beat" (e.g. an eighth note in 6/8) spans this many quarter-note beats.
        const double signatureBeatLength = 4.0 / static_cast<double>(denominator);
        if (signatureBeatLength <= 0.0)
            continue;

        const double regionBeats = std::isfinite(region.endBeat)
            ? std::max(0.0, region.endBeat - region.startBeat) : 0.0;
        // Round a partial trailing bar UP. A signature region that ends mid-bar -- which happens
        // whenever the master clip's meta events do not start on a bar line -- would otherwise
        // hand the next region a bar number this region has already used, printing the same
        // number twice a few pixels apart. (The doubled bar *line* that pairing also produces is
        // a tempo-map question, not a ruler one, and is left alone here.)
        const long long regionBars = std::isfinite(region.endBeat)
            ? static_cast<long long>(std::ceil(
                  regionBeats / (signatureBeatLength * numerator) - 1e-9))
            : 0;

        if (region.endBeat > startBeat && region.startBeat < endBeat) {
            const double pixelsPerSignatureBeat = signatureBeatLength * pixelsPerBeat;
            const bool drawSubBeatLines = pixelsPerSignatureBeat >= 3.0;
            // When even whole bars would crowd together, thin them out rather than drawing a
            // solid block of lines.
            const double pixelsPerBar = pixelsPerSignatureBeat * numerator;
            long long barStride = 1;
            while (pixelsPerBar * static_cast<double>(barStride) < 4.0 && barStride < (1LL << 20))
                barStride *= 2;
            const bool labelBars = pixelsPerBar * static_cast<double>(barStride) >= 48.0 * g.uiScale;

            const double regionEnd = std::min(region.endBeat, endBeat);
            const double visibleStart = std::max(region.startBeat, startBeat);
            long long index = static_cast<long long>(
                std::floor((visibleStart - region.startBeat) / signatureBeatLength));

            for (; drawn < kMaxRulerLines; ++drawn, ++index) {
                const double beatPos = region.startBeat +
                    static_cast<double>(index) * signatureBeatLength;
                if (beatPos > regionEnd + 1e-9)
                    break;
                if (beatPos < visibleStart - 1e-9)
                    continue;

                const bool isBar = (index % static_cast<long long>(numerator)) == 0;
                const long long barIndex = index / static_cast<long long>(numerator);
                if (isBar) {
                    if (barIndex % barStride != 0)
                        continue;
                } else if (!drawSubBeatLines) {
                    continue;
                }

                const float x = g.contentMinX +
                    static_cast<float>((beatPos - startBeat) * pixelsPerBeat);
                if (x < g.contentMinX || x > g.contentMaxX)
                    continue;

                drawList->AddLine(ImVec2(x, g.headerMaxY), ImVec2(x, g.contentMaxY),
                                  isBar ? barColor : beatColor,
                                  isBar ? barThickness : beatThickness);

                if (headerHeight <= 0.0f)
                    continue;
                const float tickTop = g.headerMaxY - headerHeight * (isBar ? 0.5f : 0.25f);
                drawList->AddLine(ImVec2(x, tickTop), ImVec2(x, g.headerMaxY), tickColor, 1.0f);
                if (isBar && labelBars) {
                    // Bars are numbered from 1, the way every other tool counts them.
                    const auto label = std::format("{}", barsBefore + barIndex + 1);
                    drawList->AddText(ImVec2(x + 3.0f * g.uiScale, g.headerMinY), tickColor,
                                      label.c_str());
                }
            }
        }

        if (!std::isfinite(region.endBeat) || region.startBeat >= endBeat)
            break;
        barsBefore += regionBars;
    }

    drawList->PopClipRect();
}

} // namespace uapmd_app_gui
