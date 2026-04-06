#include "ClipPreview.hpp"

#include <algorithm>
#include <limits>
#include <cmath>
#include <format>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <umppi/umppi.hpp>
#include <uapmd-data/uapmd-data.hpp>

#include "../AppModel.hpp"
#include <imgui.h>

namespace uapmd::gui {
namespace {

constexpr size_t kWaveformResolution = 512;
constexpr float kNodeContentPadding = 5.0f;
constexpr float kLabelSpacing = 2.0f;
constexpr float kMinimumNoteHeight = 4.0f;
constexpr double kMinimumNoteDuration = 0.01;
constexpr ImU32 kMarkerColor = IM_COL32(255, 222, 89, 220);
constexpr ImU32 kWarpColor = IM_COL32(255, 120, 120, 230);

ImVec4 withAlpha(const ImVec4& color, float alpha) {
    return ImVec4(color.x, color.y, color.z, alpha);
}

ImVec4 mixColor(const ImVec4& a, const ImVec4& b, float t) {
    return ImLerp(a, b, t);
}

int64_t secondsToSamples(double seconds, double sampleRate) {
    return static_cast<int64_t>(std::llround(seconds * sampleRate));
}

double samplesToSeconds(int64_t samples, double sampleRate) {
    if (sampleRate <= 0.0)
        return 0.0;
    return static_cast<double>(samples) / sampleRate;
}

std::string markerDisplayName(const uapmd::ClipMarker& marker, size_t index) {
    if (!marker.name.empty())
        return marker.name;
    if (!marker.markerId.empty())
        return marker.markerId;
    return std::format("Marker {}", index + 1);
}

std::optional<std::string> buildWarpReferenceLabel(
    const uapmd::ClipData& clipData,
    const uapmd::AudioWarpPoint& warp
) {
    auto& appModel = uapmd::AppModel::instance();
    const auto tracks = appModel.getTimelineTracks();

    auto findClipByReferenceId = [&](std::string_view referenceId) -> std::optional<uapmd::ClipData> {
        if (referenceId.empty())
            return clipData;
        for (auto* track : tracks) {
            if (!track)
                continue;
            const auto clips = track->clipManager().getAllClips();
            auto it = std::find_if(clips.begin(), clips.end(), [referenceId](const auto& candidate) {
                return candidate.referenceId == referenceId;
            });
            if (it != clips.end())
                return *it;
        }
        return std::nullopt;
    };

    switch (warp.referenceType) {
        case uapmd::AudioWarpReferenceType::Manual:
            return std::nullopt;
        case uapmd::AudioWarpReferenceType::ClipStart:
            return std::string("Start");
        case uapmd::AudioWarpReferenceType::ClipEnd:
            return std::string("End");
        case uapmd::AudioWarpReferenceType::ClipMarker: {
            const auto refClip = findClipByReferenceId(warp.referenceClipId);
            if (!refClip)
                return std::nullopt;
            for (size_t i = 0; i < refClip->markers.size(); ++i) {
                const auto& marker = refClip->markers[i];
                if (marker.markerId == warp.referenceMarkerId)
                    return markerDisplayName(marker, i);
            }
            return std::nullopt;
        }
        case uapmd::AudioWarpReferenceType::MasterMarker: {
            const auto& masterMarkers = appModel.masterTrackMarkers();
            for (size_t i = 0; i < masterMarkers.size(); ++i) {
                const auto& marker = masterMarkers[i];
                if (marker.markerId == warp.referenceMarkerId)
                    return markerDisplayName(marker, i);
            }
            return std::nullopt;
        }
    }

    return std::nullopt;
}

std::unordered_map<std::string, uapmd::ClipData> buildClipLookup() {
    std::unordered_map<std::string, uapmd::ClipData> clipLookup;
    for (auto* track : uapmd::AppModel::instance().getTimelineTracks()) {
        if (!track)
            continue;
        auto clips = track->clipManager().getAllClips();
        for (auto& clip : clips)
            clipLookup[clip.referenceId] = std::move(clip);
    }
    return clipLookup;
}

constexpr std::string_view kMasterMarkerReferenceId = "master_track";

const uapmd::ClipMarker* findMarkerById(const std::vector<uapmd::ClipMarker>& markers, std::string_view markerId) {
    auto it = std::find_if(markers.begin(), markers.end(), [markerId](const auto& marker) {
        return marker.markerId == markerId;
    });
    return it == markers.end() ? nullptr : &(*it);
}

struct MarkerKey {
    std::string clipReferenceId;
    std::string markerId;

    bool operator==(const MarkerKey& other) const {
        return clipReferenceId == other.clipReferenceId && markerId == other.markerId;
    }
};

struct MarkerKeyHash {
    size_t operator()(const MarkerKey& key) const {
        size_t seed = std::hash<std::string>{}(key.clipReferenceId);
        seed ^= std::hash<std::string>{}(key.markerId) + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
        return seed;
    }
};

std::optional<int64_t> resolveMarkerAbsoluteSample(
    std::string_view ownerReferenceId,
    const uapmd::ClipMarker& marker,
    const std::unordered_map<std::string, uapmd::ClipData>& clipLookup,
    const std::vector<uapmd::ClipMarker>& masterTrackMarkers,
    std::unordered_map<MarkerKey, std::optional<int64_t>, MarkerKeyHash>& cache,
    std::unordered_set<MarkerKey, MarkerKeyHash>& resolving
);

std::optional<int64_t> resolveReferenceAbsoluteSample(
    std::string_view ownerReferenceId,
    const uapmd::ClipData* ownerClip,
    const uapmd::TimeReference& reference,
    const std::unordered_map<std::string, uapmd::ClipData>& clipLookup,
    const std::vector<uapmd::ClipMarker>& masterTrackMarkers,
    std::unordered_map<MarkerKey, std::optional<int64_t>, MarkerKeyHash>& cache,
    std::unordered_set<MarkerKey, MarkerKeyHash>& resolving
) {
    switch (reference.type) {
        case uapmd::TimeReferenceType::ContainerStart: {
            const std::string effectiveReferenceId = reference.referenceId.empty()
                ? std::string(ownerReferenceId)
                : reference.referenceId;
            if (effectiveReferenceId == kMasterMarkerReferenceId)
                return 0;
            if (effectiveReferenceId == ownerReferenceId)
                return ownerClip ? std::optional<int64_t>(ownerClip->position.samples) : std::optional<int64_t>(0);
            if (auto it = clipLookup.find(effectiveReferenceId); it != clipLookup.end())
                return it->second.position.samples;
            return std::nullopt;
        }
        case uapmd::TimeReferenceType::ContainerEnd: {
            const std::string effectiveReferenceId = reference.referenceId.empty()
                ? std::string(ownerReferenceId)
                : reference.referenceId;
            if (effectiveReferenceId == kMasterMarkerReferenceId)
                return std::nullopt;
            if (effectiveReferenceId == ownerReferenceId)
                return ownerClip ? std::optional<int64_t>(ownerClip->position.samples + ownerClip->durationSamples) : std::nullopt;
            if (auto it = clipLookup.find(effectiveReferenceId); it != clipLookup.end())
                return it->second.position.samples + it->second.durationSamples;
            return std::nullopt;
        }
        case uapmd::TimeReferenceType::Point: {
            std::string containerReferenceId;
            std::string pointReferenceId;
            if (!uapmd::TimeReference::parsePointReferenceId(reference.referenceId, containerReferenceId, pointReferenceId))
                return std::nullopt;
            if (containerReferenceId == kMasterMarkerReferenceId) {
                auto* refMarker = findMarkerById(masterTrackMarkers, pointReferenceId);
                if (!refMarker)
                    return std::nullopt;
                return resolveMarkerAbsoluteSample(kMasterMarkerReferenceId, *refMarker, clipLookup, masterTrackMarkers, cache, resolving);
            }
            auto clipIt = clipLookup.find(containerReferenceId);
            if (clipIt == clipLookup.end())
                return std::nullopt;
            auto* refMarker = findMarkerById(clipIt->second.markers, pointReferenceId);
            if (!refMarker)
                return std::nullopt;
            return resolveMarkerAbsoluteSample(containerReferenceId, *refMarker, clipLookup, masterTrackMarkers, cache, resolving);
        }
    }
    return std::nullopt;
}

std::optional<int64_t> resolveMarkerAbsoluteSample(
    std::string_view ownerReferenceId,
    const uapmd::ClipMarker& marker,
    const std::unordered_map<std::string, uapmd::ClipData>& clipLookup,
    const std::vector<uapmd::ClipMarker>& masterTrackMarkers,
    std::unordered_map<MarkerKey, std::optional<int64_t>, MarkerKeyHash>& cache,
    std::unordered_set<MarkerKey, MarkerKeyHash>& resolving
) {
    MarkerKey key{std::string(ownerReferenceId), marker.markerId};
    if (auto it = cache.find(key); it != cache.end())
        return it->second;
    if (!resolving.insert(key).second)
        return std::nullopt;
    const uapmd::ClipData* ownerClip = nullptr;
    if (ownerReferenceId != kMasterMarkerReferenceId) {
        if (auto clipIt = clipLookup.find(std::string(ownerReferenceId)); clipIt != clipLookup.end())
            ownerClip = &clipIt->second;
    }
    auto absoluteReferenceSample = resolveReferenceAbsoluteSample(ownerReferenceId, ownerClip,
        marker.timeReference(ownerReferenceId, kMasterMarkerReferenceId), clipLookup, masterTrackMarkers, cache, resolving);
    resolving.erase(key);
    if (!absoluteReferenceSample) {
        cache[key] = std::nullopt;
        return std::nullopt;
    }
    const double sampleRate = std::max(1.0, static_cast<double>(uapmd::AppModel::instance().sampleRate()));
    cache[key] = *absoluteReferenceSample + secondsToSamples(marker.clipPositionOffset, sampleRate);
    return cache[key];
}

std::optional<int64_t> resolveMarkerClipPosition(
    const uapmd::ClipData& clipData,
    const uapmd::ClipMarker& marker,
    const std::unordered_map<std::string, uapmd::ClipData>& clipLookup,
    const std::vector<uapmd::ClipMarker>& masterTrackMarkers
) {
    std::unordered_map<MarkerKey, std::optional<int64_t>, MarkerKeyHash> cache;
    std::unordered_set<MarkerKey, MarkerKeyHash> resolving;
    auto absoluteSample = resolveMarkerAbsoluteSample(clipData.referenceId, marker, clipLookup, masterTrackMarkers, cache, resolving);
    if (!absoluteSample)
        return std::nullopt;
    const int64_t clipPosition = *absoluteSample - clipData.position.samples;
    if (clipPosition < 0 || clipPosition > clipData.durationSamples)
        return std::nullopt;
    return clipPosition;
}

std::optional<int64_t> resolveWarpClipPosition(
    const uapmd::ClipData& clipData,
    const uapmd::AudioWarpPoint& warp,
    const std::unordered_map<std::string, uapmd::ClipData>& clipLookup,
    const std::vector<uapmd::ClipMarker>& masterTrackMarkers
) {
    std::unordered_map<MarkerKey, std::optional<int64_t>, MarkerKeyHash> cache;
    std::unordered_set<MarkerKey, MarkerKeyHash> resolving;
    auto absoluteReferenceSample = resolveReferenceAbsoluteSample(clipData.referenceId, &clipData,
        warp.timeReference(clipData.referenceId, kMasterMarkerReferenceId), clipLookup, masterTrackMarkers, cache, resolving);
    if (!absoluteReferenceSample)
        return std::nullopt;
    const double sampleRate = std::max(1.0, static_cast<double>(uapmd::AppModel::instance().sampleRate()));
    const int64_t clipPosition = *absoluteReferenceSample + secondsToSamples(warp.clipPositionOffset, sampleRate) - clipData.position.samples;
    if (clipPosition < 0 || clipPosition > clipData.durationSamples)
        return std::nullopt;
    return clipPosition;
}

class ClipContentNode : public CustomNodeBase {
public:
    ClipContentNode(std::shared_ptr<ClipPreview> preview, float uiScale, std::string clipName)
        : preview_(std::move(preview)), uiScale_(uiScale), clipName_(std::move(clipName)) {}

    void OnDraw(const TimelineNode& nodeData, ImRect drawArea, bool& refIsSelected) override {
        ImGui::SetCursorScreenPos(drawArea.Min);
        drawBackground(nodeData, drawArea, refIsSelected);
        drawContent(drawArea);
        ImGui::Dummy(drawArea.GetSize());
    }

private:
    void drawBackground(const TimelineNode& nodeData, const ImRect& area, bool selected) const {
        auto* drawList = ImGui::GetWindowDrawList();
        ImU32 bgStart = nodeData.displayProperties.mBackgroundColor;
        ImU32 bgEnd = nodeData.displayProperties.mBackgroundColorTwo;
        drawList->AddRectFilledMultiColor(area.Min, area.Max, bgStart, bgStart, bgEnd, bgEnd);

        ImU32 borderColor = selected
            ? ImGui::GetColorU32(ImGuiCol_HeaderActive)
            : nodeData.displayProperties.mForegroundColor;
        float borderThickness = nodeData.displayProperties.BorderThickness;
        drawList->AddRect(
            area.Min,
            area.Max - ImVec2(borderThickness, borderThickness),
            borderColor,
            nodeData.displayProperties.BorderRadius,
            0,
            borderThickness
        );
    }

    void drawContent(const ImRect& area) const {
        auto* drawList = ImGui::GetWindowDrawList();
        const ImGuiStyle& style = ImGui::GetStyle();
        ImRect padded = area;
        float padding = kNodeContentPadding * uiScale_;
        padded.Min.x += padding;
        padded.Min.y += padding;
        padded.Max.x -= padding;
        padded.Max.y -= padding;
        if (padded.Min.x >= padded.Max.x || padded.Min.y >= padded.Max.y) {
            return;
        }

        // Copy the title string to avoid dangling pointer if preview_ is modified during render
        std::string titleStr = clipName_.empty()
            ? (preview_ && !preview_->displayName.empty() ? preview_->displayName : "Clip")
            : clipName_;
        drawList->AddText(padded.Min, ImGui::GetColorU32(withAlpha(style.Colors[ImGuiCol_Text], 0.92f)), titleStr.c_str());

        std::string typeLabel = (preview_ && preview_->isMidiClip) ? "MIDI" : "Audio";
        ImVec2 labelSize = ImGui::CalcTextSize(typeLabel.c_str());
        drawList->AddText(
            ImVec2(padded.Max.x - labelSize.x, padded.Min.y),
            ImGui::GetColorU32(withAlpha(style.Colors[ImGuiCol_TextDisabled], 0.92f)),
            typeLabel.c_str()
        );

        ImRect contentRect = padded;
        contentRect.Min.y += ImGui::GetFontSize() + kLabelSpacing * uiScale_;
        if (contentRect.Min.y >= contentRect.Max.y) {
            return;
        }

        if (!preview_) {
            drawPlaceholder(contentRect, "Loading...");
            return;
        }
        if (preview_->hasError) {
            drawPlaceholder(contentRect, preview_->errorMessage.c_str());
            return;
        }
        if (!preview_->ready) {
            drawPlaceholder(contentRect, "Preparing preview...");
            return;
        }

        if (preview_->isMasterMeta) {
            drawMasterMeta(contentRect);
        } else if (preview_->isMidiClip) {
            drawMidi(contentRect);
        } else {
            drawWaveform(contentRect);
        }
    }

    void drawPlaceholder(const ImRect& rect, const char* text) const {
        auto* drawList = ImGui::GetWindowDrawList();
        drawList->PushClipRect(rect.Min, rect.Max, true);
        ImVec2 textSize = ImGui::CalcTextSize(text ? text : "");
        ImVec2 center(
            rect.Min.x + ((rect.Max.x - rect.Min.x) - textSize.x) * 0.5f,
            rect.Min.y + ((rect.Max.y - rect.Min.y) - textSize.y) * 0.5f
        );
        drawList->AddText(center, ImGui::GetColorU32(withAlpha(ImGui::GetStyle().Colors[ImGuiCol_TextDisabled], 0.92f)), text ? text : "");
        drawList->PopClipRect();
    }

    void drawWaveform(const ImRect& rect) const {
        auto* drawList = ImGui::GetWindowDrawList();
        drawList->PushClipRect(rect.Min, rect.Max, true);

        if (preview_->waveform.empty()) {
            drawPlaceholder(rect, "No audio data");
            drawList->PopClipRect();
            return;
        }

        const float width = rect.Max.x - rect.Min.x;
        const float height = rect.Max.y - rect.Min.y;
        if (width <= 0.0f || height <= 0.0f) {
            drawList->PopClipRect();
            return;
        }

        const float centerY = rect.Min.y + height * 0.5f;
        const float halfHeight = height * 0.5f;
        const ImGuiStyle& style = ImGui::GetStyle();
        const ImU32 lineColor = ImGui::GetColorU32(withAlpha(mixColor(style.Colors[ImGuiCol_Text], style.Colors[ImGuiCol_HeaderActive], 0.35f), 0.70f));
        const double safeDurationSeconds = std::max(0.001, preview_->clipDurationSeconds);

        const size_t count = preview_->waveform.size();
        for (size_t i = 0; i < count; ++i) {
            const auto& point = preview_->waveform[i];
            if (!point.hasData) {
                continue;
            }

            float t = count > 1 ? static_cast<float>(i) / static_cast<float>(count - 1) : 0.0f;
            float x = rect.Min.x + t * width;
            float maxValue = std::clamp(point.maxValue, -1.0f, 1.0f);
            float minValue = std::clamp(point.minValue, -1.0f, 1.0f);
            float y1 = centerY - maxValue * halfHeight;
            float y2 = centerY - minValue * halfHeight;

            drawList->AddLine(ImVec2(x, y1), ImVec2(x, y2), lineColor, 1.2f * uiScale_);
        }

        const float labelXOffset = 3.0f * uiScale_;
        const float labelYOffset = 3.0f * uiScale_;
        const float textLineHeight = ImGui::GetTextLineHeight();
        const float markerLabelY = rect.Min.y + labelYOffset;
        const float warpLabelY = markerLabelY + textLineHeight + labelYOffset;

        auto drawClipLine = [&](double clipPositionSeconds, ImU32 color, const char* label, float labelY) {
            double normalized = std::clamp(clipPositionSeconds / safeDurationSeconds, 0.0, 1.0);
            float x = rect.Min.x + static_cast<float>(normalized) * width;
            drawList->AddLine(ImVec2(x, rect.Min.y), ImVec2(x, rect.Max.y), color, 1.5f * uiScale_);
            if (label && label[0] != '\0') {
                drawList->AddText(ImVec2(x + labelXOffset, labelY), color, label);
            }
        };

        for (const auto& marker : preview_->clipMarkers) {
            const char* label = marker.name.empty() ? nullptr : marker.name.c_str();
            drawClipLine(marker.clipPositionOffset, kMarkerColor, label, markerLabelY);
        }

        for (size_t i = 0; i < preview_->audioWarps.size(); ++i) {
            const auto& warp = preview_->audioWarps[i];
            const char* referenceLabel = nullptr;
            if (i < preview_->audioWarpReferenceLabels.size() && !preview_->audioWarpReferenceLabels[i].empty())
                referenceLabel = preview_->audioWarpReferenceLabels[i].c_str();
            drawClipLine(warp.clipPositionOffset, kWarpColor, referenceLabel, markerLabelY);
            std::string label = std::format("{:.3f}x", warp.speedRatio);
            drawClipLine(warp.clipPositionOffset, kWarpColor, label.c_str(), warpLabelY);
        }

        drawList->PopClipRect();
    }

    void drawMidi(const ImRect& rect) const {
        auto* drawList = ImGui::GetWindowDrawList();
        drawList->PushClipRect(rect.Min, rect.Max, true);

        if (preview_->midiNotes.empty()) {
            drawPlaceholder(rect, "No MIDI events");
            drawList->PopClipRect();
            return;
        }

        const float width = rect.Max.x - rect.Min.x;
        const float height = rect.Max.y - rect.Min.y;
        if (width <= 0.0f || height <= 0.0f) {
            drawList->PopClipRect();
            return;
        }

        double duration = std::max(0.01, preview_->clipDurationSeconds);
        int noteRange = std::max(1, static_cast<int>(preview_->maxNote) - static_cast<int>(preview_->minNote) + 1);
        float laneHeight = height / static_cast<float>(noteRange);

        for (const auto& note : preview_->midiNotes) {
            double startRatio = std::clamp(note.startSeconds / duration, 0.0, 1.0);
            double endSeconds = note.startSeconds + note.durationSeconds;
            double endRatio = std::clamp(endSeconds / duration, 0.0, 1.0);
            float x1 = rect.Min.x + static_cast<float>(startRatio) * width;
            float x2 = rect.Min.x + static_cast<float>(endRatio) * width;
            if (x2 <= x1) {
                x2 = x1 + 1.5f * uiScale_;
            }

            float notePos = static_cast<float>(preview_->maxNote - note.note) / static_cast<float>(noteRange);
            float y1 = rect.Min.y + notePos * height;
            float y2 = y1 + std::max(kMinimumNoteHeight * uiScale_, laneHeight * 0.8f);
            y2 = std::min(y2, rect.Max.y);

            float intensity = note.velocity;
            ImU32 fillColor = ImGui::GetColorU32(ImVec4(0.2f + 0.5f * intensity,
                                                        0.4f + 0.3f * intensity,
                                                        0.9f - 0.4f * intensity,
                                                        0.9f));
            drawList->AddRectFilled(ImVec2(x1, y1), ImVec2(x2, y2), fillColor, 3.0f * uiScale_);
            drawList->AddRect(ImVec2(x1, y1), ImVec2(x2, y2), IM_COL32(20, 20, 20, 200), 3.0f * uiScale_, 0, 1.0f);
        }

        drawList->PopClipRect();
    }

    void drawMasterMeta(const ImRect& rect) const {
        auto* drawList = ImGui::GetWindowDrawList();
        drawList->PushClipRect(rect.Min, rect.Max, true);

        const float width = rect.Max.x - rect.Min.x;
        const float height = rect.Max.y - rect.Min.y;
        if (width <= 0.0f || height <= 0.0f) {
            drawList->PopClipRect();
            return;
        }

        if (preview_->tempoPoints.empty() && preview_->timeSignaturePoints.empty()) {
            drawPlaceholder(rect, "No meta events");
            drawList->PopClipRect();
            return;
        }

        double duration = std::max(0.001, preview_->clipDurationSeconds);
        double minBpm = std::numeric_limits<double>::max();
        double maxBpm = std::numeric_limits<double>::lowest();
        for (const auto& point : preview_->tempoPoints) {
            if (point.bpm <= 0.0)
                continue;
            minBpm = std::min(minBpm, point.bpm);
            maxBpm = std::max(maxBpm, point.bpm);
        }
        if (!std::isfinite(minBpm) || !std::isfinite(maxBpm) || minBpm >= maxBpm) {
            minBpm = 40.0;
            maxBpm = 200.0;
        }
        const double bpmRange = std::max(1.0, maxBpm - minBpm);

        auto toX = [&](double seconds) -> float {
            double normalized = std::clamp(seconds / duration, 0.0, 1.0);
            return rect.Min.x + static_cast<float>(normalized * width);
        };
        auto toY = [&](double bpm) -> float {
            double normalized = std::clamp((bpm - minBpm) / bpmRange, 0.0, 1.0);
            return rect.Max.y - static_cast<float>(normalized * height);
        };

        const ImU32 gridColor = IM_COL32(120, 120, 140, 160);
        const int gridLines = 3;
        for (int i = 0; i <= gridLines; ++i) {
            float t = static_cast<float>(i) / static_cast<float>(gridLines);
            float y = rect.Max.y - t * height;
            drawList->AddLine(ImVec2(rect.Min.x, y), ImVec2(rect.Max.x, y), gridColor, 1.0f);
        }

        const ImU32 tempoColor = IM_COL32(112, 202, 255, 255);
        const float tempoStroke = 2.0f * uiScale_;
        for (size_t i = 0; i < preview_->tempoPoints.size(); ++i) {
            const auto& point = preview_->tempoPoints[i];
            if (point.bpm <= 0.0)
                continue;

            const float startX = toX(point.timeSeconds);
            const float y = toY(point.bpm);
            float endX = toX(preview_->clipDurationSeconds);
            if (i + 1 < preview_->tempoPoints.size()) {
                endX = toX(preview_->tempoPoints[i + 1].timeSeconds);
            }

            if (endX > startX) {
                drawList->AddLine(ImVec2(startX, y), ImVec2(endX, y), tempoColor, tempoStroke);
            }
            drawList->AddCircleFilled(ImVec2(startX, y), 3.0f * uiScale_, tempoColor);

            if (i + 1 < preview_->tempoPoints.size()) {
                const auto& nextPoint = preview_->tempoPoints[i + 1];
                if (nextPoint.bpm > 0.0) {
                    const float nextY = toY(nextPoint.bpm);
                    drawList->AddLine(ImVec2(endX, y), ImVec2(endX, nextY), tempoColor, tempoStroke);
                }
            }
        }

        const ImU32 sigColor = IM_COL32(214, 143, 255, 200);
        for (const auto& sig : preview_->timeSignaturePoints) {
            float x = toX(sig.timeSeconds);
            drawList->AddLine(ImVec2(x, rect.Min.y), ImVec2(x, rect.Max.y), sigColor, 1.0f);
            std::string label = std::format("{}/{}", sig.numerator, sig.denominator);
            drawList->AddText(ImVec2(x + 4.0f, rect.Min.y + 4.0f), sigColor, label.c_str());
        }

        drawList->PopClipRect();
    }

    std::shared_ptr<ClipPreview> preview_;
    float uiScale_{1.0f};
    std::string clipName_;
};

inline double computeDurationFromClip(const uapmd::ClipData* clipData) {
    if (!clipData) {
        return 0.0;
    }

    const double sampleRate = static_cast<double>(uapmd::AppModel::instance().sampleRate());
    if (sampleRate <= 0.0) {
        return 0.0;
    }

    return static_cast<double>(clipData->durationSamples) / sampleRate;
}

} // namespace

std::shared_ptr<ClipPreview> createAudioClipPreview(
    const std::string& filepath,
    double fallbackDurationSeconds,
    const uapmd::ClipData* clipData
) {
    auto preview = std::make_shared<ClipPreview>();
    preview->isMidiClip = false;

    double durationSeconds = computeDurationFromClip(clipData);
    if (durationSeconds <= 0.0) {
        durationSeconds = fallbackDurationSeconds;
    }
    preview->clipDurationSeconds = std::max(0.001, durationSeconds);
    preview->sourceDurationSamples = clipData ? std::max<int64_t>(0, clipData->durationSamples) : 0;
    if (clipData) {
        const auto clipLookup = buildClipLookup();
        const auto& masterTrackMarkers = uapmd::AppModel::instance().masterTrackMarkers();
        preview->clipMarkers.reserve(clipData->markers.size());
        const double sampleRate = std::max(1.0, static_cast<double>(uapmd::AppModel::instance().sampleRate()));
        for (auto marker : clipData->markers) {
            if (auto resolved = resolveMarkerClipPosition(*clipData, marker, clipLookup, masterTrackMarkers))
                marker.clipPositionOffset = samplesToSeconds(*resolved, sampleRate);
            preview->clipMarkers.push_back(std::move(marker));
        }
        preview->audioWarps.reserve(clipData->audioWarps.size());
        preview->audioWarpReferenceLabels.reserve(clipData->audioWarps.size());
        for (auto warp : clipData->audioWarps) {
            auto label = buildWarpReferenceLabel(*clipData, warp);
            preview->audioWarpReferenceLabels.push_back(label.value_or(std::string{}));
            if (auto resolved = resolveWarpClipPosition(*clipData, warp, clipLookup, masterTrackMarkers))
                warp.clipPositionOffset = samplesToSeconds(*resolved, sampleRate);
            preview->audioWarps.push_back(std::move(warp));
        }
    }

    if (filepath.empty()) {
        preview->hasError = true;
        preview->errorMessage = "Audio file unavailable";
        return preview;
    }

    auto reader = uapmd::createAudioFileReaderFromPath(filepath);
    if (!reader) {
        preview->hasError = true;
        preview->errorMessage = "Failed to open audio file";
        return preview;
    }

    const auto props = reader->getProperties();
    if (props.numFrames == 0 || props.numChannels == 0) {
        preview->hasError = true;
        preview->errorMessage = "Empty audio file";
        return preview;
    }

    preview->waveform.assign(kWaveformResolution, ClipPreview::WaveformPoint{});

    const uint32_t channelCount = std::max<uint32_t>(1, props.numChannels);
    const uint64_t totalFrames = props.numFrames;
    constexpr size_t kChunkFrames = 4096;
    std::vector<std::vector<float>> channelBuffers(channelCount, std::vector<float>(kChunkFrames));
    std::vector<float*> channelPtrs(channelCount);
    for (uint32_t ch = 0; ch < channelCount; ++ch) {
        channelPtrs[ch] = channelBuffers[ch].data();
    }

    for (uint64_t frame = 0; frame < totalFrames; frame += kChunkFrames) {
        const uint64_t framesToRead = std::min<uint64_t>(kChunkFrames, totalFrames - frame);
        reader->readFrames(frame, framesToRead, channelPtrs.data(), channelCount);

        for (uint64_t i = 0; i < framesToRead; ++i) {
            double sampleValue = 0.0;
            for (uint32_t ch = 0; ch < channelCount; ++ch) {
                sampleValue += channelBuffers[ch][static_cast<size_t>(i)];
            }
            sampleValue /= static_cast<double>(channelCount);
            sampleValue = std::clamp(sampleValue, -1.0, 1.0);

            uint64_t absoluteFrame = frame + i;
            size_t bucketIndex = 0;
            if (totalFrames > 1) {
                double ratio = static_cast<double>(absoluteFrame) / static_cast<double>(totalFrames - 1);
                bucketIndex = static_cast<size_t>(ratio * static_cast<double>(kWaveformResolution - 1));
            }
            bucketIndex = std::min(bucketIndex, kWaveformResolution - 1);

            auto& peak = preview->waveform[bucketIndex];
            if (!peak.hasData) {
                peak.minValue = static_cast<float>(sampleValue);
                peak.maxValue = static_cast<float>(sampleValue);
                peak.hasData = true;
            } else {
                peak.minValue = std::min(peak.minValue, static_cast<float>(sampleValue));
                peak.maxValue = std::max(peak.maxValue, static_cast<float>(sampleValue));
            }
        }
    }

    for (auto& peak : preview->waveform) {
        if (!peak.hasData) {
            peak.minValue = 0.0f;
            peak.maxValue = 0.0f;
        }
    }

    preview->ready = true;
    return preview;
}

std::shared_ptr<ClipPreview> createMidiClipPreview(
    int32_t trackIndex,
    const uapmd::ClipData& clipData,
    double fallbackDurationSeconds
) {
    auto preview = std::make_shared<ClipPreview>();
    preview->isMidiClip = true;

    double durationSeconds = computeDurationFromClip(&clipData);
    if (durationSeconds <= 0.0) {
        durationSeconds = fallbackDurationSeconds;
    }
    preview->clipDurationSeconds = std::max(0.01, durationSeconds);

    auto tracks = uapmd::AppModel::instance().getTimelineTracks();
    if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(tracks.size())) {
        preview->hasError = true;
        preview->errorMessage = "Track unavailable";
        return preview;
    }

    auto* track = tracks[trackIndex];
    if (!track) {
        preview->hasError = true;
        preview->errorMessage = "Track unavailable";
        return preview;
    }

    auto sourceNode = track->getSourceNode(clipData.sourceNodeInstanceId);
    auto* midiSource = dynamic_cast<uapmd::MidiClipSourceNode*>(sourceNode.get());
    if (!midiSource) {
        preview->hasError = true;
        preview->errorMessage = "Missing MIDI source";
        return preview;
    }

    // Capture raw UMP data for piano-roll write-back before parsing into notes.
    auto rawData = std::make_shared<ClipPreview::RawMidiData>();
    rawData->umpEvents       = midiSource->umpEvents();
    rawData->tickTimestamps  = midiSource->eventTimestampsTicks();
    rawData->tickResolution  = clipData.tickResolution > 0
                               ? clipData.tickResolution
                               : midiSource->tickResolution();
    rawData->clipTempo       = midiSource->clipTempo();
    preview->rawMidiData = rawData;

    const auto& events = midiSource->umpEvents();
    const auto& timestamps = midiSource->eventTimestampsSamples();
    if (events.empty() || timestamps.empty()) {
        preview->ready = true;
        return preview;
    }

    const double safeSampleRate = std::max(1.0, static_cast<double>(uapmd::AppModel::instance().sampleRate()));
    const size_t eventCount = std::min(events.size(), timestamps.size());
    // Maps (group<<12|channel<<7|note) -> index in preview->midiNotes for in-flight notes.
    std::unordered_map<uint32_t, size_t> activeNoteIndices;
    activeNoteIndices.reserve(64);

    // Iterate messages, advancing by getSizeInInts() to handle multi-word MIDI2 messages.
    // Only NoteOn/NoteOff are processed here; automation events are parsed on-demand
    // by the piano-roll editor directly from rawMidiData.
    size_t i = 0;
    while (i < eventCount) {
        umppi::Ump ump1(events[i]);
        const int wordCount = ump1.getSizeInInts();
        const size_t safeCount = std::min(static_cast<size_t>(wordCount), eventCount - i);
        umppi::Ump ump = (safeCount >= 2) ? umppi::Ump(events[i], events[i + 1]) : ump1;

        const double eventSeconds = static_cast<double>(timestamps[i]) / safeSampleRate;
        const auto msgType = ump.getMessageType();

        if (msgType == umppi::MessageType::MIDI1) {
            const uint8_t status = ump.getStatusCode();
            const uint8_t channel = ump.getChannelInGroup();
            const uint8_t group = ump.getGroup();

            if (status == umppi::MidiChannelStatus::NOTE_ON || status == umppi::MidiChannelStatus::NOTE_OFF) {
                const uint8_t noteNum = ump.getMidi1Note();
                const uint8_t velocity = ump.getMidi1Velocity();
                const uint32_t key = (static_cast<uint32_t>(group) << 12) |
                                     (static_cast<uint32_t>(channel) << 7) | noteNum;
                const bool isNoteOn = (status == umppi::MidiChannelStatus::NOTE_ON) && velocity > 0;
                if (isNoteOn) {
                    ClipPreview::MidiNote note{};
                    note.startSeconds  = eventSeconds;
                    note.note          = noteNum;
                    note.velocity      = velocity / 127.0f;
                    note.channel       = channel;
                    note.isMidi2       = false;
                    note.noteOnWordIdx = i;
                    activeNoteIndices[key] = preview->midiNotes.size();
                    preview->midiNotes.push_back(std::move(note));
                } else {
                    auto it = activeNoteIndices.find(key);
                    if (it != activeNoteIndices.end()) {
                        auto& n = preview->midiNotes[it->second];
                        n.durationSeconds = std::max(kMinimumNoteDuration, eventSeconds - n.startSeconds);
                        n.noteOffWordIdx  = i;
                        activeNoteIndices.erase(it);
                    }
                }
            }
        } else if (msgType == umppi::MessageType::MIDI2) {
            const uint8_t status = ump.getStatusCode();
            const uint8_t channel = ump.getChannelInGroup();
            const uint8_t group = ump.getGroup();

            if (status == umppi::MidiChannelStatus::NOTE_ON || status == umppi::MidiChannelStatus::NOTE_OFF) {
                const uint8_t  noteNum = ump.getMidi2Note();
                const uint16_t vel16   = ump.getMidi2Velocity16();
                const uint32_t key = (static_cast<uint32_t>(group) << 12) |
                                     (static_cast<uint32_t>(channel) << 7) | noteNum;
                const bool isNoteOn = (status == umppi::MidiChannelStatus::NOTE_ON) && vel16 > 0;
                if (isNoteOn) {
                    ClipPreview::MidiNote note{};
                    note.startSeconds  = eventSeconds;
                    note.note          = noteNum;
                    note.velocity      = vel16 / 65535.0f;
                    note.channel       = channel;
                    note.isMidi2       = true;
                    note.noteOnWordIdx = i;
                    activeNoteIndices[key] = preview->midiNotes.size();
                    preview->midiNotes.push_back(std::move(note));
                } else {
                    auto it = activeNoteIndices.find(key);
                    if (it != activeNoteIndices.end()) {
                        auto& n = preview->midiNotes[it->second];
                        n.durationSeconds = std::max(kMinimumNoteDuration, eventSeconds - n.startSeconds);
                        n.noteOffWordIdx  = i;
                        activeNoteIndices.erase(it);
                    }
                }
            }
        }

        i += static_cast<size_t>(std::max(1, wordCount));
    }

    // Finalize any notes that had no matching NoteOff.
    for (auto& [key, idx] : activeNoteIndices) {
        auto& n = preview->midiNotes[idx];
        n.durationSeconds = std::max(kMinimumNoteDuration, preview->clipDurationSeconds - n.startSeconds);
    }

    uint8_t minNote = 127;
    uint8_t maxNote = 0;
    for (const auto& note : preview->midiNotes) {
        minNote = std::min(minNote, note.note);
        maxNote = std::max(maxNote, note.note);
    }

    if (!preview->midiNotes.empty()) {
        preview->minNote = minNote;
        preview->maxNote = maxNote;
    }

    preview->ready = true;
    return preview;
}

std::shared_ptr<CustomNodeBase> createClipContentNode(
    std::shared_ptr<ClipPreview> preview,
    float uiScale,
    const std::string& clipName
) {
    return std::make_shared<ClipContentNode>(std::move(preview), uiScale, clipName);
}

std::shared_ptr<ClipPreview> createMasterMetaPreview(
    std::vector<ClipPreview::TempoPoint> tempoPoints,
    std::vector<ClipPreview::TimeSignaturePoint> timeSignaturePoints,
    double durationSeconds
) {
    auto preview = std::make_shared<ClipPreview>();
    preview->isMasterMeta = true;
    preview->clipDurationSeconds = std::max(0.001, durationSeconds);
    preview->tempoPoints = std::move(tempoPoints);
    preview->timeSignaturePoints = std::move(timeSignaturePoints);
    preview->ready = true;
    return preview;
}

} // namespace uapmd::gui
