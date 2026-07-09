#include "BeatsSequenceEditor.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>
#include <ImTimeline.h>
#include "TrackLegendNodeView.hpp"
#include "TimelineLaneAssignment.hpp"
#include "BeatsTimelineConstants.hpp"
#include "uapmd-data/detail/timeline/MidiClipSourceNode.hpp"

#include <uapmd-app-model/uapmd-app-model.hpp>
#include "ClipPreview.hpp"
#include "ContextActions.hpp"

namespace uapmd::gui {

namespace {

constexpr float kTimelineSectionSpacing = 5.0f; // Matches Timeline::DrawTimeline spacing
constexpr float kTimelineChildPadding = 8.0f;   // Matches Timeline::DrawTimeline padding
constexpr ImU32 kTimelinePlayheadColor = IM_COL32(255, 230, 0, 255);

ImVec4 withAlpha(const ImVec4& color, float alpha) {
    return ImVec4(color.x, color.y, color.z, alpha);
}

ImVec4 mixColor(const ImVec4& a, const ImVec4& b, float t) {
    return ImLerp(a, b, t);
}

uint64_t mixHash(uint64_t seed, uint64_t value) {
    constexpr uint64_t kPrime = 1099511628211ull;
    seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
    seed *= kPrime;
    return seed;
}

uint64_t midiSourceFingerprint(const uapmd::MidiClipSourceNode& midiSource) {
    const auto& words = midiSource.umpEvents();
    const auto& ticks = midiSource.eventTimestampsTicks();
    uint64_t hash = 1469598103934665603ull;
    hash = mixHash(hash, words.size());
    hash = mixHash(hash, ticks.size());
    hash = mixHash(hash, static_cast<uint64_t>(midiSource.tickResolution()));
    hash = mixHash(hash, static_cast<uint64_t>(midiSource.clipTempo() * 1000.0));

    const size_t sampleCount = std::min<size_t>(words.size(), 24);
    for (size_t i = 0; i < sampleCount; ++i) {
        size_t index = (sampleCount <= 1 || words.size() <= 1)
            ? 0
            : (i * (words.size() - 1)) / (sampleCount - 1);
        hash = mixHash(hash, words[index]);
        if (index < ticks.size())
            hash = mixHash(hash, ticks[index]);
    }

    return hash;
}

uint64_t clipWarpFingerprint(const uapmd::ClipData& clipData) {
    uint64_t hash = 1469598103934665603ull;
    hash = mixHash(hash, clipData.markers.size());
    for (const auto& marker : clipData.markers) {
        const auto reference = marker.timeReference(clipData.referenceId, "master_track");
        hash = mixHash(hash, std::bit_cast<uint64_t>(marker.clipPositionOffset));
        hash = mixHash(hash, static_cast<uint64_t>(reference.type));
        for (unsigned char ch : reference.referenceId)
            hash = mixHash(hash, ch);
        for (unsigned char ch : marker.markerId)
            hash = mixHash(hash, ch);
        for (unsigned char ch : marker.name)
            hash = mixHash(hash, ch);
    }

    hash = mixHash(hash, clipData.audioWarps.size());
    for (const auto& warp : clipData.audioWarps) {
        const auto reference = warp.timeReference(clipData.referenceId, "master_track");
        hash = mixHash(hash, std::bit_cast<uint64_t>(warp.clipPositionOffset));
        hash = mixHash(hash, std::bit_cast<uint64_t>(warp.speedRatio));
        hash = mixHash(hash, static_cast<uint64_t>(reference.type));
        for (unsigned char ch : reference.referenceId)
            hash = mixHash(hash, ch);
    }
    return hash;
}

double secondsToBeats(const BeatsSequenceEditor::RenderContext& context, double seconds) {
    if (context.secondsToBeats)
        return context.secondsToBeats(seconds);
    return seconds;
}

double beatsToSeconds(const BeatsSequenceEditor::RenderContext& context, double beats) {
    if (context.beatsToSeconds)
        return context.beatsToSeconds(beats);
    return beats;
}

} // namespace

BeatsSequenceEditor::~BeatsSequenceEditor() = default;

void BeatsSequenceEditor::refreshClips(int32_t trackIndex, const std::vector<ClipRow>& clips) {
    auto& state = tracks_[trackIndex];
    state.displayClips = clips;
    unified_.dirty = true;
    pruneClipPreviewCache(state);
}

void BeatsSequenceEditor::removeStaleWindows(int32_t maxValidTrackIndex) {
    bool removed = false;
    for (auto it = tracks_.begin(); it != tracks_.end();) {
        if (it->first > maxValidTrackIndex) {
            it = tracks_.erase(it);
            removed = true;
        } else {
            ++it;
        }
    }
    if (removed)
        unified_.dirty = true;
}

void BeatsSequenceEditor::invalidateTimeline() {
    unified_.dirty = true;
}

float BeatsSequenceEditor::getUnifiedTimelineHeight(float uiScale) const {
    const float minHeight = 120.0f * uiScale;
    if (unified_.computedTimelineHeight > 0.0f)
        return std::max(unified_.computedTimelineHeight, minHeight);
    const float baseSectionHeight = std::max(80.0f * uiScale, 40.0f);
    const auto sectionCount = static_cast<float>(tracks_.size());
    const float headerHeight = static_cast<float>(static_cast<int>(24.0f * uiScale));
    const float sectionsHeight = sectionCount * baseSectionHeight;
    const float spacingHeight = sectionCount * kTimelineSectionSpacing;
    const float computedHeight = headerHeight + sectionsHeight + spacingHeight + kTimelineChildPadding;
    return std::max(computedHeight, minHeight);
}

void BeatsSequenceEditor::reset() {
    tracks_.clear();
    unified_ = UnifiedTimelineState{};
}

void BeatsSequenceEditor::renderNavigator(const RenderContext& context, float barStartScreenX) {
    // Rebuild eagerly so the navigator works even when it renders before the timeline widget
    // (it lives in the always-visible toolbar row, outside the track scroll area).
    if (unified_.dirty)
        rebuildUnifiedTimeline(context);
    if (!unified_.timeline)
        return;

    auto& appModel = uapmd::AppModel::instance();
    const auto bounds = appModel.timelineContentBounds();
    const int32_t sampleRate = appModel.sampleRate();
    const double contentBeats = bounds.hasContent
        ? secondsToBeats(context, bounds.durationSeconds) : 0.0;
    const double playheadBeats = sampleRate > 0
        ? secondsToBeats(context, appModel.timeline().playheadPosition.toSeconds(sampleRate)) : -1.0;

    // Whole-song overview lanes, in the same track order the timeline sections use
    // (sorted track index; kMasterTrackIndex naturally sorts first).
    std::vector<int32_t> sortedTracks;
    sortedTracks.reserve(tracks_.size());
    for (const auto& [trackIndex, _] : tracks_)
        sortedTracks.push_back(trackIndex);
    std::sort(sortedTracks.begin(), sortedTracks.end());
    std::vector<NavigatorClip> overview;
    for (size_t row = 0; row < sortedTracks.size(); ++row)
        for (const auto& clip : tracks_[sortedTracks[row]].displayClips)
            overview.push_back({static_cast<int>(row),
                                static_cast<double>(clip.timelineStartTicks),
                                static_cast<double>(clip.timelineEndTicks)});

    renderTimelineNavigator(*unified_.timeline, unified_.hasExplicitZoom,
                            context.uiScale, barStartScreenX,
                            contentBeats * kTicksPerBeatDisplay,
                            playheadBeats * kTicksPerBeatDisplay,
                            unified_.lastVisibleWidthPixels,
                            overview, static_cast<int>(sortedTracks.size()));
}

void BeatsSequenceEditor::renderUnifiedTimeline(const RenderContext& context, float availableHeight) {
    if (availableHeight <= 0.0f)
        availableHeight = ImGui::GetContentRegionAvail().y;
    availableHeight = std::max(availableHeight, 120.0f * context.uiScale);

    if (unified_.dirty)
        rebuildUnifiedTimeline(context);

    if (!unified_.timeline) {
        ImGui::TextUnformatted("Unable to build timeline.");
        return;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    if (ImGui::BeginChild("##UnifiedBeatsTimeline", ImVec2(0, availableHeight), true,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        const bool timelineHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
        const ImVec2 winPos = ImGui::GetWindowPos();
        const ImVec2 winSize = ImGui::GetWindowSize();
        const float clipAreaMinX = winPos.x + unified_.style.LegendWidth;
        const float clipAreaMinY = winPos.y + static_cast<float>(unified_.style.HeaderHeight);
        const float clipAreaMaxX = winPos.x + winSize.x;
        const float clipAreaMaxY = winPos.y + winSize.y;
        unified_.lastVisibleWidthPixels = std::max(0.0f, clipAreaMaxX - clipAreaMinX);
        if (unified_.hasPendingFit && unified_.lastVisibleWidthPixels > 0.0f) {
            unified_.hasPendingFit = false;
            fitToContent(unified_.pendingFitDurationBeats, unified_.lastVisibleWidthPixels, unified_.pendingFitUiScale);
        }

        const bool popupBlocking = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
        ImGuiIO& io = ImGui::GetIO();
        const bool anyMouseActivity = io.MouseDown[0] || io.MouseDown[1] ||
                                      io.MouseWheel != 0.0f || io.MouseWheelH != 0.0f;
        const bool scrollbarDragging = unified_.timeline->GetLastInputData().IsMovingScrollBar;
        // Exempt only items owned by this timeline window (see SequenceEditor.cpp): items
        // active in other windows (e.g. a dragged overlay) must not disable blocking.
        const ImGuiWindow* timelineWindow = ImGui::GetCurrentWindow();
        bool timelineItemActive = false;
        if (ImGuiContext* ctx = ImGui::GetCurrentContext(); ctx->ActiveId != 0)
            for (const ImGuiWindow* w = ctx->ActiveIdWindow; w; w = w->ParentWindow)
                if (w == timelineWindow) {
                    timelineItemActive = true;
                    break;
                }
        const bool shouldBlockInput = !scrollbarDragging && (popupBlocking || (!timelineItemActive && !timelineHovered && anyMouseActivity));

        bool savedMouseDown[5];
        float savedMouseWheel = 0.0f, savedMouseWheelH = 0.0f;
        if (shouldBlockInput) {
            for (int i = 0; i < 5; ++i) { savedMouseDown[i] = io.MouseDown[i]; io.MouseDown[i] = false; }
            savedMouseWheel = io.MouseWheel; savedMouseWheelH = io.MouseWheelH;
            io.MouseWheel = 0.0f; io.MouseWheelH = 0.0f;
        }

        unified_.timeline->DrawTimeline();

        if (shouldBlockInput) {
            for (int i = 0; i < 5; ++i) io.MouseDown[i] = savedMouseDown[i];
            io.MouseWheel = savedMouseWheel; io.MouseWheelH = savedMouseWheelH;
        }

        if (clipAreaMaxX > clipAreaMinX && clipAreaMaxY > clipAreaMinY)
            drawBeatGridOverlay(context, clipAreaMinX, clipAreaMinY, clipAreaMaxX, clipAreaMaxY);

        if (clipAreaMaxX > clipAreaMinX && clipAreaMinY > winPos.y)
            drawPlayheadIndicator(context, clipAreaMinX, winPos.y, clipAreaMaxX, clipAreaMinY);

        const ImVec2 headerMousePos = ImGui::GetMousePos();
        if (timelineHovered &&
            headerMousePos.x >= clipAreaMinX && headerMousePos.x <= clipAreaMaxX &&
            headerMousePos.y >= winPos.y && headerMousePos.y < clipAreaMinY &&
            ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            const float scale = unified_.timeline->GetScale();
            if (scale > 0.0f) {
                const double displayTicks =
                    static_cast<double>(unified_.timeline->GetStartTimestamp()) +
                    static_cast<double>((headerMousePos.x - clipAreaMinX) / scale);
                uapmd::AppModel::instance().transport().jump(
                    beatsToSeconds(context, displayTicks / kTicksPerBeatDisplay));
            }
        }

        // Drag tracking
        if (unified_.timeline->mDragData.DragState == eDragState::DragNode &&
            unified_.activeDragNodeId == InvalidNodeID && !shouldBlockInput && timelineHovered)
            unified_.activeDragNodeId = unified_.timeline->mDragData.DragNode.GetID();
        if (unified_.activeDragNodeId != InvalidNodeID && shouldBlockInput)
            unified_.activeDragNodeId = InvalidNodeID;

        // Drag completion
        if (unified_.activeDragNodeId != InvalidNodeID &&
            unified_.timeline->mDragData.DragState == eDragState::None &&
            !unified_.timeline->IsDragging()) {
            const NodeID nodeId = unified_.activeDragNodeId;
            unified_.activeDragNodeId = InvalidNodeID;
            auto clipIt = unified_.nodeToClip.find(nodeId);
            if (clipIt != unified_.nodeToClip.end() && clipIt->second.clipId >= 0) {
                auto* node = unified_.timeline->FindNodeByNodeID(nodeId);
                if (node && context.moveClipAbsolute) {
                    const double newStartBeats = static_cast<double>(node->start) / kTicksPerBeatDisplay;
                    const double newStartSeconds = beatsToSeconds(context, newStartBeats);
                    context.moveClipAbsolute(clipIt->second.trackIndex, clipIt->second.clipId, newStartSeconds);
                }
            }
        }

        // Determine hovered track from selected section
        const int32_t selectedSection = unified_.timeline->GetSelectedSection();
        int32_t hoveredTrackIndex = -1;
        if (selectedSection >= 0 && selectedSection < static_cast<int32_t>(unified_.sectionToTrack.size()))
            hoveredTrackIndex = unified_.sectionToTrack[static_cast<size_t>(selectedSection)];

        // Double-click interaction
        const ImVec2 mousePos = ImGui::GetMousePos();
        const bool mouseInClipArea = mousePos.x >= clipAreaMinX && mousePos.x <= clipAreaMaxX &&
                                     mousePos.y >= clipAreaMinY && mousePos.y <= clipAreaMaxY;

        // Shared hit-testing helpers, used by both the double-click and range-drag interactions.
        auto sectionTopYFor = [&](int32_t trackIdx) -> float {
            float y = clipAreaMinY;
            for (int32_t si = 0; si < static_cast<int32_t>(unified_.sectionToTrack.size()); ++si) {
                if (unified_.sectionToTrack[static_cast<size_t>(si)] == trackIdx)
                    break;
                y += unified_.timeline->GetSectionDisplayProperties(si).mHeight + kTimelineSectionSpacing;
            }
            return y;
        };
        auto sectionHeightFor = [&](int32_t trackIdx) -> float {
            for (int32_t si = 0; si < static_cast<int32_t>(unified_.sectionToTrack.size()); ++si)
                if (unified_.sectionToTrack[static_cast<size_t>(si)] == trackIdx)
                    return unified_.timeline->GetSectionDisplayProperties(si).mHeight;
            return 0.0f;
        };
        auto isOverClipNode = [&](int32_t trackIdx, ImVec2 pos, float scale, double startFrame, int32_t* outClipId) -> bool {
            const float sectionTopY = sectionTopYFor(trackIdx);
            const float sfOffset = clipAreaMinX - static_cast<float>(startFrame) * scale;
            for (const auto& [nodeId, ref] : unified_.nodeToClip) {
                if (ref.trackIndex != trackIdx) continue;
                auto* node = unified_.timeline->FindNodeByNodeID(nodeId);
                if (!node) continue;
                const float nMinX = sfOffset + static_cast<float>(node->start) * scale;
                const float nMaxX = sfOffset + static_cast<float>(node->end + 1) * scale;
                // Also check Y so overlapping clips in different lanes are disambiguated.
                const float nodeH = node->displayProperties.mHeight > 0.0f
                    ? node->displayProperties.mHeight
                    : sectionHeightFor(trackIdx);
                const float nMinY = sectionTopY + node->displayProperties.yOffset;
                const float nMaxY = nMinY + nodeH;
                if (pos.x >= std::max(nMinX, clipAreaMinX) && pos.x <= std::min(nMaxX, clipAreaMaxX) &&
                    pos.y >= nMinY && pos.y <= nMaxY) {
                    if (outClipId) *outClipId = ref.clipId;
                    return true;
                }
            }
            return false;
        };

        int32_t requestedContextTrack = -1;
        int32_t requestedAddClipTrack = -1;

        if (timelineHovered && mouseInClipArea && hoveredTrackIndex != -1 &&
            ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            auto& trackState = tracks_[hoveredTrackIndex];
            const float scale = unified_.timeline->GetScale();
            const double startFrame = static_cast<double>(unified_.timeline->GetStartTimestamp());
            bool clipUnderMouse = false;
            if (scale > 0.0f) {
                int32_t hitClipId = -1;
                clipUnderMouse = isOverClipNode(hoveredTrackIndex, mousePos, scale, startFrame, &hitClipId);
                if (clipUnderMouse) {
                    trackState.contextMenuClipId = hitClipId;
                    requestedContextTrack = hoveredTrackIndex;
                }
            }
            // Always record the clicked timeline position so "Add New Clip Here" works
            // even when the click lands on an existing clip.
            if (scale > 0.0f) {
                const float clippedX = std::clamp(mousePos.x, clipAreaMinX, clipAreaMaxX);
                const double clickedBeats = (startFrame + static_cast<double>((clippedX - clipAreaMinX) / scale)) / kTicksPerBeatDisplay;
                const double maxSec = beatsToSeconds(context, static_cast<double>(unified_.timeline->GetMaxFrame()) / kTicksPerBeatDisplay);
                trackState.requestedAddPosition = std::clamp(beatsToSeconds(context, clickedBeats), 0.0, maxSec);
                if (!clipUnderMouse)
                    requestedAddClipTrack = hoveredTrackIndex;
            }
        }

        // Range-selection drag: click-drag across empty space (not on an existing node) within
        // a track's lane selects a time range, offering "Add New MIDI Clip"/"Add Empty Audio
        // Clip" sized to that range on release, snapped to the nearest quarter-note beat.
        int32_t requestedRangeTrack = -1;
        {
            const float scale = unified_.timeline->GetScale();
            const double startFrame = static_cast<double>(unified_.timeline->GetStartTimestamp());
            if (scale > 0.0f) {
                const float clippedX = std::clamp(mousePos.x, clipAreaMinX, clipAreaMaxX);
                const int32_t frameUnderMouse = static_cast<int32_t>(std::llround(
                    startFrame + static_cast<double>((clippedX - clipAreaMinX) / scale)));

                const bool mouseClicked = timelineHovered && mouseInClipArea && hoveredTrackIndex != -1 &&
                    !shouldBlockInput && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                    unified_.timeline->mDragData.DragState == eDragState::None;
                const bool overNode = mouseClicked && isOverClipNode(hoveredTrackIndex, mousePos, scale, startFrame, nullptr);

                updateRangeSelectionDrag(unified_.rangeDrag, ImGui::IsMouseDown(ImGuiMouseButton_Left),
                                          mouseClicked, overNode, hoveredTrackIndex, frameUnderMouse);

                if (unified_.rangeDrag.active) {
                    drawRangeSelectionOverlay(clipAreaMinX, scale, static_cast<float>(startFrame),
                        sectionTopYFor(unified_.rangeDrag.trackIndex), sectionHeightFor(unified_.rangeDrag.trackIndex));
                }

                if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && unified_.rangeDrag.active) {
                    int32_t rTrack = -1, rStart = 0, rEnd = 0;
                    const float pixelsDragged = std::abs(static_cast<float>(
                        unified_.rangeDrag.currentFrame - unified_.rangeDrag.anchorFrame)) * scale;
                    if (finishRangeSelectionDrag(unified_.rangeDrag, pixelsDragged, 4.0f * context.uiScale,
                                                  rTrack, rStart, rEnd)) {
                        auto& trackState = tracks_[rTrack];
                        // Snap to the nearest quarter-note beat before converting to seconds.
                        const double startBeats = std::round(static_cast<double>(rStart) / kTicksPerBeatDisplay);
                        double endBeats = std::round(static_cast<double>(rEnd) / kTicksPerBeatDisplay);
                        if (endBeats <= startBeats)
                            endBeats = startBeats + 1.0;
                        const double maxSec = beatsToSeconds(context, static_cast<double>(unified_.timeline->GetMaxFrame()) / kTicksPerBeatDisplay);
                        trackState.requestedRangeStart = std::clamp(beatsToSeconds(context, startBeats), 0.0, maxSec);
                        trackState.requestedRangeEnd = std::clamp(beatsToSeconds(context, endBeats), 0.0, maxSec);
                        requestedRangeTrack = rTrack;
                    }
                }
            }
        }

        // Open & render per-track context menus
        for (int32_t trackIndex : unified_.sectionToTrack) {
            auto it = tracks_.find(trackIndex);
            if (it == tracks_.end()) continue;
            auto& trackState = it->second;

            const std::string clipPopupId = std::format("BeatsTimelineClipContext##{}", trackIndex);
            const std::string addPopupId  = std::format("BeatsTimelineAddClipContext##{}", trackIndex);
            const std::string rangePopupId = std::format("BeatsTimelineRangeAddContext##{}", trackIndex);

            if (trackIndex == requestedContextTrack)
                ImGui::OpenPopup(clipPopupId.c_str());
            if (trackIndex == requestedAddClipTrack)
                ImGui::OpenPopup(addPopupId.c_str());
            if (trackIndex == requestedRangeTrack)
                ImGui::OpenPopup(rangePopupId.c_str());

            if (ImGui::BeginPopup(clipPopupId.c_str())) {
                const ClipRow* contextClip = nullptr;
                if (trackState.contextMenuClipId != -1) {
                    auto rowIt = std::find_if(trackState.displayClips.begin(), trackState.displayClips.end(),
                        [id = trackState.contextMenuClipId](const ClipRow& r) { return r.clipId == id; });
                    if (rowIt != trackState.displayClips.end())
                        contextClip = &(*rowIt);
                }
                if (!contextClip) {
                    ImGui::TextDisabled("Clip not available.");
                } else {
                    const bool canDump = (contextClip->isMasterTrack && static_cast<bool>(context.showMasterTrackDump)) ||
                                        (contextClip->isMidiClip && static_cast<bool>(context.showMidiClipDump));
                    if (!canDump) ImGui::BeginDisabled();
                    if (contextActionMenuItem("Show Dump List")) {
                        if (contextClip->isMasterTrack && context.showMasterTrackDump) context.showMasterTrackDump();
                        else if (context.showMidiClipDump) context.showMidiClipDump(trackIndex, contextClip->clipId);
                        ImGui::CloseCurrentPopup();
                    }
                    if (!canDump) ImGui::EndDisabled();

                    const bool canAudio = !contextClip->isMasterTrack && !contextClip->isMidiClip && static_cast<bool>(context.showAudioClipEvents);
                    if (!canAudio) ImGui::BeginDisabled();
                    if (contextActionMenuItem("Edit Audio Events")) {
                        if (context.showAudioClipEvents) context.showAudioClipEvents(trackIndex, contextClip->clipId);
                        ImGui::CloseCurrentPopup();
                    }
                    if (!canAudio) ImGui::EndDisabled();

                    const bool canRoll = contextClip->isMidiClip && static_cast<bool>(context.showPianoRoll);
                    if (!canRoll) ImGui::BeginDisabled();
                    if (contextActionMenuItem("Open Piano Roll")) {
                        if (context.showPianoRoll) context.showPianoRoll(trackIndex, contextClip->clipId);
                        ImGui::CloseCurrentPopup();
                    }
                    if (!canRoll) ImGui::EndDisabled();

                    const bool canDelete = static_cast<bool>(context.removeClip);
                    if (!canDelete) ImGui::BeginDisabled();
                    if (contextActionMenuItem("Delete")) {
                        if (context.removeClip) context.removeClip(trackIndex, contextClip->clipId);
                        ImGui::CloseCurrentPopup();
                    }
                    if (!canDelete) ImGui::EndDisabled();

                    if (!contextClip->isMasterTrack) {
                        ImGui::Separator();
                        if (contextActionMenuItem("New Clip Here")) {
                            if (context.addBlankMidiClipAtPosition)
                                context.addBlankMidiClipAtPosition(trackIndex, trackState.requestedAddPosition);
                            ImGui::CloseCurrentPopup();
                        }
                        if (contextActionMenuItem("Import Audio Clip Here...")) {
                            if (context.addAudioClip)
                                context.addAudioClip(trackIndex, trackState.requestedAddPosition);
                            ImGui::CloseCurrentPopup();
                        }
                        if (contextActionMenuItem("Import SMF Here...")) {
                            if (context.addSmfClip)
                                context.addSmfClip(trackIndex, trackState.requestedAddPosition);
                            ImGui::CloseCurrentPopup();
                        }
                    }
                }
                ImGui::EndPopup();
            }

            if (ImGui::BeginPopup(addPopupId.c_str())) {
                const bool isMasterTrack = (trackIndex == uapmd::kMasterTrackIndex);
                if (contextActionMenuItem("Edit Clips...")) {
                    if (context.showClipsWindow) context.showClipsWindow(trackIndex);
                    ImGui::CloseCurrentPopup();
                }
                if (!isMasterTrack) {
                    ImGui::Separator();
                    if (contextActionMenuItem("New Clip")) {
                        if (context.addBlankMidiClipAtPosition)
                            context.addBlankMidiClipAtPosition(trackIndex, trackState.requestedAddPosition);
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::Separator();
                    if (contextActionMenuItem("Import Audio Clip...")) {
                        if (context.addAudioClip) context.addAudioClip(trackIndex, trackState.requestedAddPosition);
                        ImGui::CloseCurrentPopup();
                    }
                }
                ImGui::Separator();
                if (contextActionMenuItem("Import SMF as Clip...")) {
                    if (context.addSmfClip) context.addSmfClip(trackIndex, trackState.requestedAddPosition);
                    ImGui::CloseCurrentPopup();
                }
                if (!isMasterTrack) {
                    if (contextActionMenuItem("Import SMF2Clip...")) {
                        if (context.addSmf2Clip) context.addSmf2Clip(trackIndex);
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::Separator();
                    if (contextActionMenuItem("Clear All")) {
                        if (context.clearAllClips) context.clearAllClips(trackIndex);
                        ImGui::CloseCurrentPopup();
                    }
                }
                ImGui::EndPopup();
            }

            if (trackIndex != uapmd::kMasterTrackIndex && ImGui::BeginPopup(rangePopupId.c_str())) {
                if (contextActionMenuItem("Add New MIDI Clip")) {
                    if (context.addBlankMidiClipInRange)
                        context.addBlankMidiClipInRange(trackIndex, trackState.requestedRangeStart, trackState.requestedRangeEnd);
                    ImGui::CloseCurrentPopup();
                }
                if (contextActionMenuItem("Add Empty Audio Clip")) {
                    if (context.addEmptyAudioClipInRange)
                        context.addEmptyAudioClipInRange(trackIndex, trackState.requestedRangeStart, trackState.requestedRangeEnd);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

void BeatsSequenceEditor::rebuildUnifiedTimeline(const RenderContext& context) {
    // rebuildUnifiedTimeline replaces the Timeline object outright, so an explicit zoom has to be
    // captured from the outgoing object and re-applied to the new one -- hasExplicitZoom alone
    // only prevents the *default* from being set below, it doesn't carry the value across.
    const float preservedScale = (unified_.timeline && unified_.hasExplicitZoom)
        ? unified_.timeline->GetScale() : -1.0f;
    unified_.timeline = std::make_unique<ImTimeline::Timeline>();
    unified_.nodeToClip.clear();
    unified_.activeDragNodeId = InvalidNodeID;
    unified_.sectionToTrack.clear();

    unified_.timeline->mFlags.set(TimelineFlags_SkipTimelineRebuild, true);

    ImTimelineStyle style;
    const ImGuiStyle& imguiStyle = ImGui::GetStyle();
    const ImVec4 frameBg = imguiStyle.Colors[ImGuiCol_FrameBg];
    const ImVec4 childBg = imguiStyle.Colors[ImGuiCol_ChildBg];
    const ImVec4 windowBg = imguiStyle.Colors[ImGuiCol_WindowBg];
    const ImVec4 header = imguiStyle.Colors[ImGuiCol_Header];
    const ImVec4 headerHovered = imguiStyle.Colors[ImGuiCol_HeaderHovered];
    const ImVec4 text = imguiStyle.Colors[ImGuiCol_Text];
    const ImVec4 textDisabled = imguiStyle.Colors[ImGuiCol_TextDisabled];

    style.LegendWidth = context.legendWidth;
    style.HeaderHeight = static_cast<int>(24.0f * context.uiScale);
    style.ScrollbarThickness = static_cast<int>(12.0f * context.uiScale);
    style.SeekbarWidth = 2.0f * context.uiScale;
    style.HasSeekbar = false;
    // The position controller in the navigation row above the timeline supersedes ImTimeline's
    // built-in bottom scrollbar (its thumb drag was the same operation); disable to avoid two
    // competing pan controls and reclaim the vertical space.
    style.HasScrollbar = false;
    style.HeaderBackgroundColor = ImGui::GetColorU32(mixColor(frameBg, windowBg, 0.35f));
    style.HeaderTimeStampColor = ImGui::GetColorU32(withAlpha(textDisabled, 1.0f));
    style.LegendTextColor = ImGui::GetColorU32(withAlpha(textDisabled, 1.0f));
    style.SectionBackgroundColor = ImGui::GetColorU32(mixColor(childBg, frameBg, 0.65f));
    style.SectionBackgroundHoveredColor = ImGui::GetColorU32(withAlpha(headerHovered, 0.18f));
    style.SelectedNodeOutlineColor = ImGui::GetColorU32(mixColor(headerHovered, text, 0.15f));
    style.NodeHoveredOverlayColor = ImGui::GetColorU32(withAlpha(headerHovered, 0.14f));
    style.ScrollbarBackgroundColor = ImGui::GetColorU32(mixColor(windowBg, childBg, 0.55f));
    style.ScrollbarTrackColor = ImGui::GetColorU32(mixColor(frameBg, childBg, 0.75f));
    style.ScrollbarThumbColor = ImGui::GetColorU32(withAlpha(textDisabled, 0.55f));
    style.ScrollbarThumbHoveredColor = ImGui::GetColorU32(withAlpha(textDisabled, 0.8f));
    style.ScrollbarHandleColor = ImGui::GetColorU32(withAlpha(text, 0.18f));
    style.ScrollbarHandleHoveredColor = ImGui::GetColorU32(withAlpha(text, 0.32f));
    style.SeekbarColor = ImGui::GetColorU32(withAlpha(text, 0.9f));
    unified_.timeline->SetTimelineStyle(style);
    unified_.style = style;

    const float baseHeight = std::max(80.0f * context.uiScale, 40.0f);

    // Sort tracks: kMasterTrackIndex (INT32_MIN) naturally sorts first
    std::vector<int32_t> sortedTracks;
    sortedTracks.reserve(tracks_.size());
    for (const auto& [trackIndex, _] : tracks_)
        sortedTracks.push_back(trackIndex);
    std::sort(sortedTracks.begin(), sortedTracks.end());
    unified_.sectionToTrack = sortedTracks;

    int32_t maxFrame = 0;
    float totalSectionHeight = 0.0f;
    const float headerHeight = static_cast<float>(static_cast<int>(24.0f * context.uiScale));

    for (int32_t sectionIdx = 0; sectionIdx < static_cast<int32_t>(sortedTracks.size()); ++sectionIdx) {
        const int32_t trackIndex = sortedTracks[static_cast<size_t>(sectionIdx)];
        auto& state = tracks_[trackIndex];

        std::string sectionName = (trackIndex == uapmd::kMasterTrackIndex)
            ? std::string("Master Track")
            : std::format("Track {}", trackIndex + 1);

        if (context.renderLegendContent) {
            auto legendView = std::make_shared<TrackLegendNodeView>();
            legendView->trackIndex = trackIndex;
            legendView->renderContent = context.renderLegendContent;
            unified_.timeline->InitializeTimelineSectionEx(sectionIdx, sectionName, nullptr, nullptr, legendView);
        } else {
            unified_.timeline->InitializeTimelineSection(sectionIdx, sectionName);
        }
        unified_.timeline->SetTimelineName(sectionIdx, sectionName);

        // Greedy lane assignment distributes overlapping clips into separate vertical lanes
        // within the same section (see TimelineLaneAssignment.hpp for the algorithm).
        std::vector<LaneAssignmentInput> laneInputs;
        laneInputs.reserve(state.displayClips.size());
        for (const auto& c : state.displayClips)
            laneInputs.push_back({c.clipId, c.timelineStartTicks, c.timelineEndTicks});
        int numLanes = 1;
        std::unordered_map<int32_t, int> clipLaneMap = assignLanes(laneInputs, numLanes);
        const float laneHeight = baseHeight;
        const float sectionHeight = numLanes * laneHeight;

        auto& props = unified_.timeline->GetSectionDisplayProperties(sectionIdx);
        props.mHeight = sectionHeight;
        props.mBackgroundColor = ImGui::GetColorU32(mixColor(frameBg, header, 0.20f));
        props.mBackgroundColorTwo = ImGui::GetColorU32(mixColor(childBg, header, 0.10f));
        props.mForegroundColor = ImGui::GetColorU32(withAlpha(text, 1.0f));
        props.BorderRadius = 6.0f * context.uiScale;
        props.BorderThickness = 1.0f;
        props.AccentThickness = 8;
        unified_.timeline->SetTimelineHeight(sectionIdx, sectionHeight);
        totalSectionHeight += sectionHeight + kTimelineSectionSpacing;

        for (const auto& clip : state.displayClips) {
            const int lane = clipLaneMap.count(clip.clipId) ? clipLaneMap.at(clip.clipId) : 0;

            TimelineNode node;
            node.Setup(sectionIdx, clip.timelineStartTicks, clip.timelineEndTicks, sectionName);
            node.displayProperties = props;
            node.displayProperties.yOffset = lane * laneHeight;
            node.mFlags.set(eTimelineNodeFlags::TimelineNodeFlags_UseSectionBackground, true);
            if (numLanes > 1) {
                node.mFlags.set(eTimelineNodeFlags::TimelineNodeFlags_AutofitHeight, false);
                node.displayProperties.mHeight = laneHeight - 2.0f; // 2 px gap between lanes
            } else {
                node.mFlags.set(eTimelineNodeFlags::TimelineNodeFlags_AutofitHeight, true);
            }
            node.mFlags.set(eTimelineNodeFlags::TimelineNodeFlags_MoveSurroundingNodesToTheRight, false);
            node.mFlags.set(eTimelineNodeFlags::TimelineNodeFlags_MovedToDifferentTimeline, true);

            auto preview = ensureClipPreview(trackIndex, clip, state);
            auto clipLabel = clip.name.empty() ? std::format("Clip {}", clip.clipId) : clip.name;
            auto customNode = createClipContentNode(preview, context.uiScale, clipLabel);
            if (customNode)
                node.InitalizeCustomNode(customNode);

            auto* addedNode = unified_.timeline->AddNewNode(&node);
            if (addedNode) {
                addedNode->mFlags.set(eTimelineNodeFlags::TimelineNodeFlags_MovedToDifferentTimeline, false);
                addedNode->mFlags.set(eTimelineNodeFlags::TimelineNodeFlags_MoveSurroundingNodesToTheRight, false);
                unified_.nodeToClip[addedNode->GetID()] = {trackIndex, clip.clipId};
            }
            maxFrame = std::max(maxFrame, clip.timelineEndTicks);
        }
    }

    unified_.computedTimelineHeight = headerHeight + totalSectionHeight + kTimelineChildPadding;

    if (maxFrame <= 0) maxFrame = 4 * kTicksPerBeatDisplay; // default to a few empty bars
    unified_.timeline->SetStartFrame(0);
    unified_.timeline->SetMaxFrame(maxFrame + 4 * kTicksPerBeatDisplay);
    // Once the user (or fitToContent) has set an explicit zoom, ordinary rebuilds (triggered by
    // any clip add/move/remove) must not reset it back to the default -- carry the prior scale
    // forward onto the new Timeline object instead.
    if (preservedScale >= 0.0f) {
        unified_.timeline->SetScale(preservedScale);
    } else {
        // Target ~24 px per quarter-note beat at 1.0 uiScale (down from an earlier, much more
        // zoomed-in default that only showed a couple of bars at once).
        unified_.timeline->SetScale(std::clamp((24.0f * context.uiScale) / kTicksPerBeatDisplay,
            kMinSafeTimelineScale, kMaxTimelineScale));
    }
    unified_.dirty = false;
}

void BeatsSequenceEditor::fitToContent(double contentDurationBeats, float visibleWidthPixels, float uiScale) {
    if (contentDurationBeats <= 0.0)
        return;
    // No render has happened yet to know the real visible width (e.g. fitting right after a
    // project load, before the timeline widget's first frame) -- defer until renderUnifiedTimeline
    // knows the actual width, rather than silently doing nothing.
    if (!unified_.timeline || visibleWidthPixels <= 0.0f) {
        unified_.hasPendingFit = true;
        unified_.pendingFitDurationBeats = contentDurationBeats;
        unified_.pendingFitUiScale = uiScale;
        return;
    }
    const float defaultScale = std::clamp((24.0f * uiScale) / kTicksPerBeatDisplay,
        kMinSafeTimelineScale, kMaxTimelineScale);
    const double contentFrames = contentDurationBeats * kTicksPerBeatDisplay;
    const float idealScale = static_cast<float>(visibleWidthPixels / contentFrames);
    const float fitted = std::min(idealScale, defaultScale);
    unified_.timeline->SetScale(fitted);
    unified_.hasExplicitZoom = true;
    unified_.hasPendingFit = false;
}

void BeatsSequenceEditor::drawBeatGridOverlay(
    const RenderContext& context,
    float clipAreaMinX,
    float clipAreaMinY,
    float clipAreaMaxX,
    float clipAreaMaxY
) const {
    if (!unified_.timeline || !context.tempoMap)
        return;

    const float scale = unified_.timeline->GetScale();
    if (scale <= 0.0f || clipAreaMaxX <= clipAreaMinX || clipAreaMaxY <= clipAreaMinY)
        return;

    const double startFrame = static_cast<double>(unified_.timeline->GetStartTimestamp());
    const double startBeat = startFrame / kTicksPerBeatDisplay;
    const double endBeat = startBeat + static_cast<double>(clipAreaMaxX - clipAreaMinX) / (scale * kTicksPerBeatDisplay);

    // Signature regions to walk: the tempo map's effective signatures, or a single implicit 4/4
    // region over the whole visible range when no time-signature meta events exist yet.
    std::vector<uapmd::TempoMap::EffectiveSignature> regions = context.tempoMap->effectiveSignatures();
    if (regions.empty() || regions.front().startBeat > 1e-9) {
        uapmd::TempoMap::EffectiveSignature defaultRegion;
        defaultRegion.startBeat = 0.0;
        defaultRegion.endBeat = regions.empty() ? std::numeric_limits<double>::infinity() : regions.front().startBeat;
        defaultRegion.numerator = 4;
        defaultRegion.denominator = 4;
        regions.insert(regions.begin(), defaultRegion);
    }

    const ImGuiStyle& imguiStyle = ImGui::GetStyle();
    const ImVec4 text = imguiStyle.Colors[ImGuiCol_Text];
    const ImU32 barColor = ImGui::GetColorU32(withAlpha(text, 0.35f));
    const ImU32 beatColor = ImGui::GetColorU32(withAlpha(text, 0.12f));
    const float barThickness = 1.5f * context.uiScale;
    const float beatThickness = 1.0f * context.uiScale;

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->PushClipRect(ImVec2(clipAreaMinX, clipAreaMinY), ImVec2(clipAreaMaxX, clipAreaMaxY), true);

    for (const auto& region : regions) {
        if (region.endBeat <= startBeat || region.startBeat >= endBeat)
            continue;

        const uint8_t numerator = region.numerator > 0 ? region.numerator : 4;
        const uint8_t denominator = region.denominator > 0 ? region.denominator : 4;
        // One "signature beat" (e.g. an eighth note in 6/8) spans this many quarter-note beats.
        const double signatureBeatLength = 4.0 / static_cast<double>(denominator);
        if (signatureBeatLength <= 0.0)
            continue;

        const double pixelsPerSignatureBeat = signatureBeatLength * kTicksPerBeatDisplay * static_cast<double>(scale);
        const bool drawSubBeatLines = pixelsPerSignatureBeat >= 3.0;

        const double regionEnd = std::min(region.endBeat, endBeat);
        const double visibleStart = std::max(region.startBeat, startBeat);
        if (visibleStart > regionEnd)
            continue;

        long long index = static_cast<long long>(std::floor((visibleStart - region.startBeat) / signatureBeatLength));
        constexpr long long kMaxLinesPerRegion = 100000; // safety guard against pathological zoom/region combos
        for (long long drawn = 0; drawn < kMaxLinesPerRegion; ++drawn, ++index) {
            const double beatPos = region.startBeat + static_cast<double>(index) * signatureBeatLength;
            if (beatPos > regionEnd + 1e-9)
                break;
            if (beatPos < visibleStart - 1e-9)
                continue;

            const bool isBar = (index % static_cast<long long>(numerator)) == 0;
            if (!isBar && !drawSubBeatLines)
                continue;

            const float x = clipAreaMinX + static_cast<float>((beatPos * kTicksPerBeatDisplay - startFrame) * static_cast<double>(scale));
            if (x < clipAreaMinX || x > clipAreaMaxX)
                continue;

            drawList->AddLine(
                ImVec2(x, clipAreaMinY), ImVec2(x, clipAreaMaxY),
                isBar ? barColor : beatColor,
                isBar ? barThickness : beatThickness);
        }
    }

    drawList->PopClipRect();
}

void BeatsSequenceEditor::drawRangeSelectionOverlay(
    float clipAreaMinX,
    float scale,
    float startFrame,
    float sectionTopY,
    float sectionHeight
) const {
    if (!unified_.rangeDrag.active || scale <= 0.0f || sectionHeight <= 0.0f)
        return;

    const float x0 = clipAreaMinX + (static_cast<float>(
        std::min(unified_.rangeDrag.anchorFrame, unified_.rangeDrag.currentFrame)) - startFrame) * scale;
    const float x1 = clipAreaMinX + (static_cast<float>(
        std::max(unified_.rangeDrag.anchorFrame, unified_.rangeDrag.currentFrame)) - startFrame) * scale;
    if (x1 <= x0)
        return;

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(ImVec2(x0, sectionTopY), ImVec2(x1, sectionTopY + sectionHeight), IM_COL32(255, 255, 255, 40));
    drawList->AddRect(ImVec2(x0, sectionTopY), ImVec2(x1, sectionTopY + sectionHeight), IM_COL32(255, 255, 255, 140), 0.0f, 0, 1.5f);
}

void BeatsSequenceEditor::drawPlayheadIndicator(
    const RenderContext& context,
    float headerMinX,
    float headerMinY,
    float headerMaxX,
    float headerMaxY
) const {
    if (!unified_.timeline)
        return;

    auto& appModel = uapmd::AppModel::instance();
    const int32_t sampleRate = appModel.sampleRate();
    if (sampleRate <= 0)
        return;

    const auto& timelineState = appModel.timeline();
    const double playheadSeconds = timelineState.playheadPosition.toSeconds(sampleRate);
    if (!std::isfinite(playheadSeconds))
        return;

    const int32_t maxFrame = unified_.timeline->GetMaxFrame();
    if (maxFrame <= 0)
        return;

    const double playheadBeats = secondsToBeats(context, playheadSeconds);
    const double playheadFrame = playheadBeats * kTicksPerBeatDisplay;
    const double clampedFrame = std::clamp(playheadFrame, 0.0, static_cast<double>(maxFrame));
    const double startFrame = static_cast<double>(unified_.timeline->GetStartTimestamp());
    if (clampedFrame < startFrame || clampedFrame > static_cast<double>(maxFrame))
        return;

    const float scale = unified_.timeline->GetScale();
    const float clipMinX = headerMinX;
    const float clipMaxX = headerMaxX;
    if (clipMaxX <= clipMinX) {
        return;
    }

    const float x = clipMinX + static_cast<float>((clampedFrame - startFrame) * static_cast<double>(scale));
    const float yTop = headerMinY;
    const float yBottom = headerMaxY;
    if (yBottom <= yTop) {
        return;
    }

    const float headerHeight = yBottom - yTop;
    const float triangleHeight = std::clamp(headerHeight * 0.75f, 6.0f, headerHeight);
    const float halfBaseWidth = std::clamp(triangleHeight * 0.35f, 4.0f, 12.0f);

    const float tipX = std::clamp(x, clipMinX, clipMaxX);
    const float baseLeftX = std::max(clipMinX, tipX - halfBaseWidth);
    const float baseRightX = std::min(clipMaxX, tipX + halfBaseWidth);
    const float tipY = std::min(yBottom, yTop + triangleHeight);

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->PushClipRect(ImVec2(clipMinX, yTop), ImVec2(clipMaxX, yBottom), true);
    const ImVec2 baseLeft(baseLeftX, yTop);
    const ImVec2 baseRight(baseRightX, yTop);
    const ImVec2 tip(tipX, tipY);
    drawList->AddTriangleFilled(baseLeft, baseRight, tip, kTimelinePlayheadColor);
    drawList->AddTriangle(baseLeft, baseRight, tip, IM_COL32(0, 0, 0, 200), 1.0f);
    drawList->PopClipRect();
}

void BeatsSequenceEditor::pruneClipPreviewCache(TrackState& state) {
    std::unordered_set<int32_t> validIds;
    validIds.reserve(state.displayClips.size());
    for (const auto& clip : state.displayClips) {
        validIds.insert(clip.clipId);
    }

    for (auto it = state.clipPreviews.begin(); it != state.clipPreviews.end();) {
        if (validIds.find(it->first) == validIds.end()) {
            it = state.clipPreviews.erase(it);
        } else {
            ++it;
        }
    }
}

const uapmd::ClipData* BeatsSequenceEditor::findClipData(int32_t trackIndex, int32_t clipId) const {
    auto tracks = uapmd::AppModel::instance().getTimelineTracks();
    if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(tracks.size())) {
        return nullptr;
    }
    auto* track = tracks[trackIndex];
    if (!track) {
        return nullptr;
    }
    return track->clipManager().getClip(clipId);
}

std::string BeatsSequenceEditor::buildClipSignature(int32_t trackIndex, const ClipRow& clip, const uapmd::ClipData* clipData) const {
    std::string sourcePath;
    if (clipData && !clipData->filepath.empty()) {
        sourcePath = clipData->filepath;
    } else {
        sourcePath = clip.filepath;
    }
    if (sourcePath.empty()) {
        sourcePath = clip.name;
    }

    const int64_t durationSamples = clipData ? clipData->durationSamples : 0;
    const int32_t sourceNodeId = clipData ? clipData->sourceNodeInstanceId : -1;
    uint64_t midiHash = 0;
    uint64_t warpHash = 0;
    if (clipData && clip.isMidiClip) {
        auto tracks = uapmd::AppModel::instance().getTimelineTracks();
        if (trackIndex >= 0 && trackIndex < static_cast<int32_t>(tracks.size()) && tracks[trackIndex]) {
            auto sourceNode = tracks[trackIndex]->getSourceNode(sourceNodeId);
            if (auto* midiSource = dynamic_cast<uapmd::MidiClipSourceNode*>(sourceNode.get()))
                midiHash = midiSourceFingerprint(*midiSource);
        }
    } else if (clipData) {
        warpHash = clipWarpFingerprint(*clipData);
    }
    return std::format("{}|{}|{}|{}|{}|{}|{}",
                       sourcePath,
                       clip.isMidiClip ? 'm' : 'a',
                       clip.timelineEndTicks - clip.timelineStartTicks,
                       durationSamples,
                       sourceNodeId,
                       midiHash,
                       warpHash);
}

std::shared_ptr<ClipPreview> BeatsSequenceEditor::ensureClipPreview(
    int32_t trackIndex,
    const ClipRow& clip,
    TrackState& state
) {
    if (clip.customPreview) {
        return clip.customPreview;
    }

    const auto* clipData = findClipData(trackIndex, clip.clipId);
    const auto signature = buildClipSignature(trackIndex, clip, clipData);
    auto existingIt = state.clipPreviews.find(clip.clipId);
    if (existingIt != state.clipPreviews.end() &&
        existingIt->second &&
        existingIt->second->signature == signature) {
        existingIt->second->displayName = clip.name;
        return existingIt->second;
    }

    // clipData already carries the exact source duration (durationSamples), which is what the
    // waveform/piano-roll preview actually needs -- no need to reconstruct it from the node's
    // beat-domain width (which, for audio clips, is a tempo-integrated *display* quantity, not a
    // 1:1 seconds measurement).
    const double sampleRate = std::max(1.0, static_cast<double>(uapmd::AppModel::instance().sampleRate()));
    const double fallbackDurationSeconds = clipData
        ? static_cast<double>(clipData->durationSamples) / sampleRate
        : 0.0;

    auto makeErrorPreview = [&](const std::string& message) {
        auto preview = std::make_shared<ClipPreview>();
        preview->isMidiClip = clip.isMidiClip;
        preview->hasError = true;
        preview->errorMessage = message;
        preview->clipDurationSeconds = fallbackDurationSeconds;
        return preview;
    };

    std::shared_ptr<ClipPreview> preview;
    if (clip.isMidiClip) {
        if (clipData) {
            preview = createMidiClipPreview(trackIndex, *clipData, fallbackDurationSeconds);
        } else {
            preview = makeErrorPreview("Clip data unavailable");
        }
    } else {
        std::string filepath = (clipData && !clipData->filepath.empty())
            ? clipData->filepath
            : clip.filepath;
        preview = createAudioClipPreview(filepath, fallbackDurationSeconds, clipData);
    }

    if (!preview) {
        preview = makeErrorPreview("Preview unavailable");
    }

    preview->signature = signature;
    preview->displayName = clip.name;
    state.clipPreviews[clip.clipId] = preview;
    return preview;
}

}
