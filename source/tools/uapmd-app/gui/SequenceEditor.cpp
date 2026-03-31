#include "SequenceEditor.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <iostream>
#include <map>
#include <unordered_map>
#include <unordered_set>

#include <imgui.h>
#include <ImTimeline.h>
#include "uapmd-data/detail/timeline/MidiClipSourceNode.hpp"

#include "../AppModel.hpp"
#include "ClipPreview.hpp"

namespace uapmd::gui {

namespace {

constexpr int32_t kTimelineSectionId = 0;
constexpr float kTimelineSectionSpacing = 5.0f; // Matches Timeline::DrawTimeline spacing
constexpr float kTimelineChildPadding = 8.0f;   // Matches Timeline::DrawTimeline padding
constexpr ImU32 kTimelinePlayheadColor = IM_COL32(255, 230, 0, 255);

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

double secondsToUnits(const SequenceEditor::RenderContext& context, double seconds) {
    if (context.secondsToTimelineUnits) {
        return context.secondsToTimelineUnits(seconds);
    }
    return seconds;
}

double unitsToSeconds(const SequenceEditor::RenderContext& context, double units) {
    if (context.timelineUnitsToSeconds) {
        return context.timelineUnitsToSeconds(units);
    }
    return units;
}

const char* timelineUnitsLabel(const SequenceEditor::RenderContext& context) {
    if (context.timelineUnitsLabel && context.timelineUnitsLabel[0] != '\0') {
        return context.timelineUnitsLabel;
    }
    return "seconds";
}
} // namespace

SequenceEditor::~SequenceEditor() = default;

void SequenceEditor::showWindow(int32_t trackIndex) {
    auto [it, inserted] = windows_.try_emplace(trackIndex);
    it->second.visible = true;
    if (inserted) {
        it->second.timelineDirty = true;
    }
}

void SequenceEditor::hideWindow(int32_t trackIndex) {
    auto it = windows_.find(trackIndex);
    if (it != windows_.end()) {
        it->second.visible = false;
    }
}

bool SequenceEditor::isVisible(int32_t trackIndex) const {
    auto it = windows_.find(trackIndex);
    return it != windows_.end() && it->second.visible;
}

void SequenceEditor::refreshClips(int32_t trackIndex, const std::vector<ClipRow>& clips) {
    auto& state = windows_[trackIndex];
    state.displayClips = clips;
    state.timelineDirty = true;
    pruneClipPreviewCache(state);
}

void SequenceEditor::removeStaleWindows(int32_t maxValidTrackIndex) {
    // Remove windows for track indices that no longer exist
    for (auto it = windows_.begin(); it != windows_.end();) {
        if (it->first > maxValidTrackIndex) {
            it = windows_.erase(it);
        } else {
            ++it;
        }
    }
}

void SequenceEditor::renderTimelineInline(int32_t trackIndex, const RenderContext& context, float availableHeight) {
    auto it = windows_.find(trackIndex);
    if (it == windows_.end()) {
        return;
    }
    renderTimelineContent(trackIndex, it->second, context, availableHeight, false);
}

float SequenceEditor::getInlineTimelineHeight(int32_t trackIndex, float uiScale) const {
    const float baseSectionHeight = std::max(80.0f * uiScale, 40.0f);
    const float minHeight = 120.0f * uiScale;

    size_t sectionCount = 1;
    ImTimelineStyle style;
    auto it = windows_.find(trackIndex);
    if (it != windows_.end()) {
        style = it->second.timelineStyle;
    } else {
        style.LegendWidth = 140.0f * uiScale;
        style.HeaderHeight = static_cast<int>(24.0f * uiScale);
        style.ScrollbarThickness = static_cast<int>(12.0f * uiScale);
        style.SeekbarWidth = 2.0f * uiScale;
    }

    const float headerHeight = static_cast<float>(style.HeaderHeight);
    const float scrollbarHeight = style.HasScrollbar ? static_cast<float>(style.ScrollbarThickness) : 0.0f;
    const float sectionsHeight = static_cast<float>(sectionCount) * baseSectionHeight;
    const float spacingHeight = static_cast<float>(sectionCount) * kTimelineSectionSpacing;
    const float computedHeight = headerHeight + sectionsHeight + spacingHeight + kTimelineChildPadding + scrollbarHeight;

    return std::max(computedHeight, minHeight);
}

void SequenceEditor::reset() {
    windows_.clear();
}

void SequenceEditor::render(const RenderContext& context) {
    // Collect track indices to avoid iterator invalidation
    std::vector<int32_t> trackIndices;
    trackIndices.reserve(windows_.size());
    for (const auto& entry : windows_) {
        trackIndices.push_back(entry.first);
    }

    for (int32_t trackIndex : trackIndices) {
        auto it = windows_.find(trackIndex);
        if (it == windows_.end()) {
            continue;
        }

        auto& state = it->second;
        if (!state.visible) {
            continue;
        }

        renderWindow(trackIndex, state, context);
    }
}

void SequenceEditor::renderWindow(int32_t trackIndex, SequenceEditorState& state, const RenderContext& context) {
    std::string windowTitle = std::format("Sequence Editor - Track {}###SequenceEditor{}", trackIndex, trackIndex);

    bool windowOpen = state.visible;
    std::string windowSizeId = std::format("SequenceEditorWindow{}", trackIndex);
    float baseWidth = 600.0f;
    const float viewportWidth = ImGui::GetIO().DisplaySize.x;
    if (viewportWidth > 0.0f && context.uiScale > 0.0f) {
        baseWidth = std::min(baseWidth, viewportWidth / context.uiScale);
    }

    if (context.setNextChildWindowSize) {
        context.setNextChildWindowSize(windowSizeId, ImVec2(baseWidth * context.uiScale, 350.0f * context.uiScale));
    }

    ImGui::SetNextWindowSizeConstraints(
        ImVec2(400.0f * context.uiScale, 150.0f * context.uiScale),
        ImVec2(FLT_MAX, FLT_MAX)
    );

    if (ImGui::Begin(windowTitle.c_str(), &windowOpen, ImGuiWindowFlags_None)) {
        if (context.updateChildWindowSizeState) {
            context.updateChildWindowSizeState(windowSizeId);
        }

        float clipRegionHeight = ImGui::GetContentRegionAvail().y;
        clipRegionHeight = std::max(150.0f * context.uiScale, clipRegionHeight);
        renderClipTable(trackIndex, state, context, clipRegionHeight);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Action buttons
        if (trackIndex >= 0) {
            if (ImGui::Button("New Clip")) {
                if (context.addClip) {
                    context.addClip(trackIndex, "");
                }
            }

            ImGui::SameLine();

            if (ImGui::Button("Clear All")) {
                if (!state.displayClips.empty()) {
                    ImGui::OpenPopup("Clear All Clips?");
                }
            }

            if (ImGui::BeginPopupModal("Clear All Clips?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::Text("This will remove all clips from this track.");
                ImGui::Text("Are you sure?");
                ImGui::Spacing();

                if (ImGui::Button("Yes, Clear All", ImVec2(120 * context.uiScale, 0))) {
                    if (context.clearAllClips) {
                        context.clearAllClips(trackIndex);
                    }
                    ImGui::CloseCurrentPopup();
                }

                ImGui::SameLine();

                if (ImGui::Button("Cancel", ImVec2(120 * context.uiScale, 0))) {
                    ImGui::CloseCurrentPopup();
                }

                ImGui::EndPopup();
            }
        } else {
            ImGui::TextDisabled("Master track editing is not available.");
        }
    }
    ImGui::End();

    if (!windowOpen) {
        state.visible = false;
    }
}

void SequenceEditor::renderClipTable(int32_t trackIndex, SequenceEditorState& state, const RenderContext& context, float availableHeight) {
    if (availableHeight <= 0.0f) {
        availableHeight = ImGui::GetContentRegionAvail().y;
    }
    availableHeight = std::max(availableHeight, 0.0f);

    std::string childId = std::format("##ClipTableRegion{}", trackIndex);
    if (ImGui::BeginChild(childId.c_str(), ImVec2(0, availableHeight), false, ImGuiWindowFlags_None)) {
        ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY;

        if (ImGui::BeginTable("ClipTable", 6, flags)) {
            ImGui::TableSetupColumn("Anchor",  ImGuiTableColumnFlags_WidthFixed, 120.0f * context.uiScale);
            ImGui::TableSetupColumn("Origin",  ImGuiTableColumnFlags_WidthFixed, 100.0f * context.uiScale);
            ImGui::TableSetupColumn("Position",  ImGuiTableColumnFlags_WidthFixed, 70.0f * context.uiScale);
            ImGui::TableSetupColumn("Name",  ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Filename",  ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Delete", ImGuiTableColumnFlags_WidthFixed, 70.0f * context.uiScale);
            ImGui::TableHeadersRow();

            for (const auto& clip : state.displayClips) {
                renderClipRow(trackIndex, clip, context);
            }

            ImGui::EndTable();
        }
    }
    ImGui::EndChild();
}

void SequenceEditor::renderTimelineContent(int32_t trackIndex, SequenceEditorState& state, const RenderContext& context, float availableHeight, bool showLabel) {
    if (availableHeight <= 0.0f) {
        availableHeight = ImGui::GetContentRegionAvail().y;
    }
    availableHeight = std::max(availableHeight, 0.0f);

    float timelineHeight = availableHeight;
    if (showLabel) {
        const float infoHeight = ImGui::GetTextLineHeightWithSpacing();
        timelineHeight -= infoHeight + ImGui::GetStyle().ItemSpacing.y;
        timelineHeight = std::max(timelineHeight, 0.0f);
        ImGui::TextDisabled("Timeline units: %s", timelineUnitsLabel(context));
        ImGui::Spacing();
    }

    if (state.timelineDirty) {
        rebuildTimelineModel(trackIndex, state, context);
    }

    if (!state.timeline) {
        ImGui::TextUnformatted("Unable to build timeline for this track.");
        return;
    }

    timelineHeight = std::max(timelineHeight, 120.0f * context.uiScale);

    std::string timelineChildId = std::format("##TimelineRegion{}", trackIndex);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    if (ImGui::BeginChild(timelineChildId.c_str(), ImVec2(0, timelineHeight), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        // Check if this window is actually hovered (will be false if another window is on top)
        const bool timelineHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
        const ImVec2 childWindowPos = ImGui::GetWindowPos();
        const ImVec2 childWindowSize = ImGui::GetWindowSize();
        const float clipAreaMinX = childWindowPos.x + state.timelineStyle.LegendWidth;
        float clipAreaMinY = childWindowPos.y + static_cast<float>(state.timelineStyle.HeaderHeight);
        const float clipAreaMaxX = childWindowPos.x + childWindowSize.x;
        float clipAreaMaxY = childWindowPos.y + childWindowSize.y;
        if (state.timelineStyle.HasScrollbar) {
            clipAreaMaxY -= static_cast<float>(state.timelineStyle.ScrollbarThickness);
        }

        // Block mouse input to ImTimeline when there's a popup/modal window on top,
        // or when the timeline child window is not hovered (another overlay window has focus).
        // ImTimeline reads directly from ImGui::GetIO(), so we temporarily clear mouse state.
        const bool popupBlocking = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
        // Suppress input if another window (overlay/dragged) has captured the mouse.
        // Check for any mouse activity (click/drag or scroll) so that scrolling an overlay
        // window does not pan the inline timeline's horizontal position.
        ImGuiIO& io = ImGui::GetIO();
        const bool anyMouseActivity = io.MouseDown[0] || io.MouseDown[1] ||
                                      io.MouseWheel != 0.0f || io.MouseWheelH != 0.0f;
        const bool overlayBlocking = !timelineHovered && anyMouseActivity;
        const bool shouldBlockInput = popupBlocking || overlayBlocking;

        bool savedMouseDown[5];
        float savedMouseWheel = 0.0f;
        float savedMouseWheelH = 0.0f;

        if (shouldBlockInput) {
            // Save and clear mouse state to prevent ImTimeline from reacting
            for (int i = 0; i < 5; ++i) {
                savedMouseDown[i] = io.MouseDown[i];
                io.MouseDown[i] = false;
            }
            savedMouseWheel = io.MouseWheel;
            savedMouseWheelH = io.MouseWheelH;
            io.MouseWheel = 0.0f;
            io.MouseWheelH = 0.0f;
        }

        state.timeline->DrawTimeline();

        // Restore mouse state
        if (shouldBlockInput) {
            for (int i = 0; i < 5; ++i)
                io.MouseDown[i] = savedMouseDown[i];
            io.MouseWheel = savedMouseWheel;
            io.MouseWheelH = savedMouseWheelH;
        }

        const float headerMinX = clipAreaMinX;
        const float headerMaxX = clipAreaMaxX;
        const float headerMinY = childWindowPos.y;
        const float headerMaxY = clipAreaMinY;
        if (headerMaxX > headerMinX && headerMaxY > headerMinY) {
            drawPlayheadIndicator(state, context, headerMinX, headerMinY, headerMaxX, headerMaxY);
        }

        // Only start tracking drag if no popup/overlay is blocking and timeline is hovered
        if (state.timeline->mDragData.DragState == eDragState::DragNode &&
            state.activeDragNodeId == InvalidNodeID &&
            !shouldBlockInput &&
            timelineHovered) {
            state.activeDragNodeId = state.timeline->mDragData.DragNode.GetID();
        }

        // Cancel active drag if a popup or overlay window captured mouse focus
        if (state.activeDragNodeId != InvalidNodeID && shouldBlockInput) {
            state.activeDragNodeId = InvalidNodeID;
        }

        int32_t requestedContextClip = -1;
        bool requestedAddClipMenu = false;
        const ImVec2 mousePos = ImGui::GetMousePos();
        const bool mouseInClipArea =
            mousePos.x >= clipAreaMinX && mousePos.x <= clipAreaMaxX &&
            mousePos.y >= clipAreaMinY && mousePos.y <= clipAreaMaxY;
        if (timelineHovered && mouseInClipArea && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            const float scale = state.timeline->GetScale();
            const double startFrame = static_cast<double>(state.timeline->GetStartTimestamp());

            bool clipUnderMouse = false;
            if (scale > 0.0f) {
                // ImTimeline's HorizontalNodeView uses s32 (integer-truncated) scale for node
                // x-positions but float scale for the mStartFrame scroll offset.  We must
                // mirror that exactly or non-zero node.start values produce large X errors.
                const float intScaleF = static_cast<float>(static_cast<int32_t>(scale));
                // sfOffset == timelinePanelRect.Min.x inside ImTimeline
                const float sfOffset = clipAreaMinX - static_cast<float>(startFrame) * scale;
                for (const auto& [nodeId, clipId] : state.nodeToClip) {
                    auto* node = state.timeline->FindNodeByNodeID(nodeId);
                    if (!node) continue;
                    // slotP1.x = sfOffset + node.start * intScale
                    // slotP2.x = sfOffset + (node.end + 1) * intScale  (ImTimeline adds +scale)
                    const float clipMinX = sfOffset + static_cast<float>(node->start) * intScaleF;
                    const float clipMaxX = sfOffset + static_cast<float>(node->end + 1) * intScaleF;
                    const float visibleClipMinX = std::max(clipMinX, clipAreaMinX);
                    const float visibleClipMaxX = std::min(clipMaxX, clipAreaMaxX);
                    if (visibleClipMinX <= visibleClipMaxX &&
                        mousePos.x >= visibleClipMinX && mousePos.x <= visibleClipMaxX) {
                        clipUnderMouse = true;
                        requestedContextClip = clipId;
                        break;
                    }
                }
            }

            if (!clipUnderMouse && scale > 0.0f && clipAreaMaxX > clipAreaMinX) {
                // Double-clicked on empty timeline area - calculate position for new clip
                const float clippedX = std::clamp(mousePos.x, clipAreaMinX, clipAreaMaxX);
                const double timestampUnits = startFrame + static_cast<double>((clippedX - clipAreaMinX) / scale);
                const double maxFrameSeconds = unitsToSeconds(context, static_cast<double>(state.timeline->GetMaxFrame()));
                const double timestampSeconds = unitsToSeconds(context, timestampUnits);
                state.requestedAddPosition = std::clamp(timestampSeconds, 0.0, maxFrameSeconds);
                requestedAddClipMenu = true;
            }
        }


        if (state.activeDragNodeId != InvalidNodeID &&
            state.timeline->mDragData.DragState == eDragState::None &&
            !state.timeline->IsDragging()) {
            const NodeID nodeId = state.activeDragNodeId;
            state.activeDragNodeId = InvalidNodeID;

            auto clipIt = state.nodeToClip.find(nodeId);
            if (clipIt != state.nodeToClip.end() && clipIt->second >= 0) {
                auto* node = state.timeline->FindNodeByNodeID(nodeId);
                if (node && context.moveClipAbsolute) {
                    double newStartUnits = static_cast<double>(node->start);
                    double newStartSeconds = unitsToSeconds(context, newStartUnits);
                    context.moveClipAbsolute(trackIndex, clipIt->second, newStartSeconds);
                }
            }
        }

        std::string popupId = std::format("TimelineClipContext##{}", trackIndex);
        if (requestedContextClip != -1) {
            state.contextMenuClipId = requestedContextClip;
            ImGui::OpenPopup(popupId.c_str());
        }

        if (ImGui::BeginPopup(popupId.c_str())) {
            const ClipRow* contextClip = nullptr;
            if (state.contextMenuClipId != -1) {
                auto clipRowIt = std::find_if(
                    state.displayClips.begin(),
                    state.displayClips.end(),
                    [clipId = state.contextMenuClipId](const ClipRow& row) { return row.clipId == clipId; }
                );
                if (clipRowIt != state.displayClips.end()) {
                    contextClip = &(*clipRowIt);
                }
            }

            if (!contextClip) {
                ImGui::TextDisabled("Clip not available.");
            } else {
                const bool canShowDump = (contextClip->isMasterTrack && static_cast<bool>(context.showMasterTrackDump)) ||
                    (contextClip->isMidiClip && static_cast<bool>(context.showMidiClipDump));
                if (!canShowDump) {
                    ImGui::BeginDisabled();
                }
                if (ImGui::MenuItem("Show Dump List")) {
                    if (contextClip->isMasterTrack) {
                        if (context.showMasterTrackDump) {
                            context.showMasterTrackDump();
                        }
                    } else if (context.showMidiClipDump) {
                        context.showMidiClipDump(trackIndex, contextClip->clipId);
                    }
                    ImGui::CloseCurrentPopup();
                }
                if (!canShowDump) {
                    ImGui::EndDisabled();
                }

                const bool canOpenAudioEvents = !contextClip->isMasterTrack &&
                                                !contextClip->isMidiClip &&
                                                static_cast<bool>(context.showAudioClipEvents);
                if (!canOpenAudioEvents)
                    ImGui::BeginDisabled();
                if (ImGui::MenuItem("Edit Audio Events")) {
                    if (context.showAudioClipEvents)
                        context.showAudioClipEvents(trackIndex, contextClip->clipId);
                    ImGui::CloseCurrentPopup();
                }
                if (!canOpenAudioEvents)
                    ImGui::EndDisabled();

                const bool canOpenPianoRoll = contextClip->isMidiClip &&
                                              static_cast<bool>(context.showPianoRoll);
                if (!canOpenPianoRoll)
                    ImGui::BeginDisabled();
                if (ImGui::MenuItem("Open Piano Roll")) {
                    if (context.showPianoRoll)
                        context.showPianoRoll(trackIndex, contextClip->clipId);
                    ImGui::CloseCurrentPopup();
                }
                if (!canOpenPianoRoll)
                    ImGui::EndDisabled();

                const bool canDelete = static_cast<bool>(context.removeClip);
                if (!canDelete) {
                    ImGui::BeginDisabled();
                }
                if (ImGui::MenuItem("Delete")) {
                    if (context.removeClip) {
                        context.removeClip(trackIndex, contextClip->clipId);
                    }
                    ImGui::CloseCurrentPopup();
                }
                if (!canDelete) {
                    ImGui::EndDisabled();
                }
            }

            ImGui::EndPopup();
        }

        // Context menu for adding new clip at specific position
        std::string addClipPopupId = std::format("TimelineAddClipContext##{}", trackIndex);
        if (requestedAddClipMenu) {
            ImGui::OpenPopup(addClipPopupId.c_str());
        }

        if (ImGui::BeginPopup(addClipPopupId.c_str())) {
            if (ImGui::MenuItem("Edit Clips...", nullptr, isVisible(trackIndex))) {
                showWindow(trackIndex);
                if (context.refreshClips)
                    context.refreshClips(trackIndex);
                ImGui::CloseCurrentPopup();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("New Clip")) {
                if (context.addBlankMidiClipAtPosition)
                    context.addBlankMidiClipAtPosition(trackIndex, state.requestedAddPosition);
                ImGui::CloseCurrentPopup();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Import Audio Clip...")) {
                if (context.addAudioClip)
                    context.addAudioClip(trackIndex);
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::MenuItem("Import SMF as Clip...")) {
                if (context.addSmfClip)
                    context.addSmfClip(trackIndex);
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::MenuItem("Import SMF2Clip...")) {
                if (context.addSmf2Clip)
                    context.addSmf2Clip(trackIndex);
                ImGui::CloseCurrentPopup();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Clear All")) {
                if (context.clearAllClips)
                    context.clearAllClips(trackIndex);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

void SequenceEditor::rebuildTimelineModel(int32_t trackIndex, SequenceEditorState& state, const RenderContext& context) {
    state.timeline = std::make_unique<ImTimeline::Timeline>();

    if (!state.timeline) {
        state.timelineDirty = false;
        return;
    }

    state.timeline->mFlags.set(TimelineFlags_SkipTimelineRebuild, true);

    ImTimelineStyle style;
    style.LegendWidth = 140.0f * context.uiScale;
    style.HeaderHeight = static_cast<int>(24.0f * context.uiScale);
    style.ScrollbarThickness = static_cast<int>(12.0f * context.uiScale);
    style.SeekbarWidth = 2.0f * context.uiScale;
    style.HasSeekbar = false;
    state.timeline->SetTimelineStyle(style);
    state.timelineStyle = style;

    const float baseHeight = std::max(80.0f * context.uiScale, 40.0f);
    int32_t maxFrame = 0;
    state.nodeToClip.clear();
    state.activeDragNodeId = InvalidNodeID;

    const int sectionIndex = 0;
    std::string sectionName = (trackIndex < 0) ? std::string("Master Track")
                                              : std::format("Track {}", trackIndex + 1);
    state.timeline->InitializeTimelineSection(sectionIndex, sectionName);
    state.timeline->SetTimelineName(sectionIndex, sectionName);

    auto& props = state.timeline->GetSectionDisplayProperties(sectionIndex);
    props.mHeight = baseHeight;
    props.mBackgroundColor = IM_COL32(63, 76, 107, 200);
    props.mBackgroundColorTwo = IM_COL32(43, 54, 86, 200);
    props.mForegroundColor = IM_COL32(255, 255, 255, 255);
    props.BorderRadius = 6.0f * context.uiScale;
    props.BorderThickness = 1.0f;
    props.AccentThickness = 8;
    state.timeline->SetTimelineHeight(sectionIndex, props.mHeight);

    for (const auto& clip : state.displayClips) {

        TimelineNode node;
        node.Setup(sectionIndex, clip.timelineStart, clip.timelineEnd, sectionName);
        node.displayProperties = props;
        node.mFlags.set(eTimelineNodeFlags::TimelineNodeFlags_UseSectionBackground, true);
        node.mFlags.set(eTimelineNodeFlags::TimelineNodeFlags_AutofitHeight, true);
        node.mFlags.set(eTimelineNodeFlags::TimelineNodeFlags_MoveSurroundingNodesToTheRight, false);
        node.mFlags.set(eTimelineNodeFlags::TimelineNodeFlags_MovedToDifferentTimeline, true);

        auto preview = ensureClipPreview(trackIndex, clip, state);
        auto clipLabel = clip.name.empty() ? std::format("Clip {}", clip.clipId) : clip.name;
        auto customNode = createClipContentNode(preview, context.uiScale, clipLabel);
        if (customNode) {
            node.InitalizeCustomNode(customNode);
        }

        auto* addedNode = state.timeline->AddNewNode(&node);
        if (addedNode) {
            addedNode->mFlags.set(eTimelineNodeFlags::TimelineNodeFlags_MovedToDifferentTimeline, false);
            addedNode->mFlags.set(eTimelineNodeFlags::TimelineNodeFlags_MoveSurroundingNodesToTheRight, false);
            state.nodeToClip[addedNode->GetID()] = clip.clipId;
        }

        maxFrame = std::max(maxFrame, clip.timelineEnd);
    }

    if (maxFrame <= 0) {
        maxFrame = 1000;
    }

    state.timeline->SetStartFrame(0);
    state.timeline->SetMaxFrame(maxFrame + 200);
    state.timeline->SetScale(std::max(1.0f, 5.0f * context.uiScale));
    state.timelineDirty = false;
}

void SequenceEditor::renderClipRow(int32_t trackIndex, const ClipRow& clip, const RenderContext& context) {
    ImGui::TableNextRow();

    bool anchorChanged = false;
    bool originChanged = false;
    bool positionChanged = false;
    bool nameChanged = false;

    // Anchor column
    ImGui::TableSetColumnIndex(0);

    ImGui::SetNextItemWidth(-FLT_MIN);  // Use all available width in column
    anchorChanged = renderAnchorCombo(trackIndex, clip, context);

    // Origin column
    ImGui::TableSetColumnIndex(1);
    originChanged = renderOriginCombo(trackIndex, clip, context);

    // Position column
    ImGui::TableSetColumnIndex(2);
    positionChanged = renderPositionInput(trackIndex, clip, context);

    // Name column (editable)
    ImGui::TableSetColumnIndex(3);
    nameChanged = renderNameInput(trackIndex, clip, context);

    // Filename column (Change button first, then filename only)
    ImGui::TableSetColumnIndex(4);
    std::string changeButtonId = std::format("Change##{}", clip.clipId);
    if (ImGui::Button(changeButtonId.c_str())) {
        if (context.changeClipFile) {
            context.changeClipFile(trackIndex, clip.clipId);
        }
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(clip.filename.c_str());

    // Delete column
    ImGui::TableSetColumnIndex(5);
    std::string deleteId = std::format("Delete##{}",  clip.clipId);
    if (ImGui::Button(deleteId.c_str())) {
        if (context.removeClip) {
            context.removeClip(trackIndex, clip.clipId);
        }
    }

    if (anchorChanged || originChanged || positionChanged || nameChanged) {
        auto refresh = context.refreshClips;
        if (refresh) {
            refresh(trackIndex);
        }
    }
}

bool SequenceEditor::renderAnchorCombo(int32_t trackIndex, const ClipRow& clip, const RenderContext& context) {
    bool changed = false;
    std::string anchorLabel = "Track";
    if (!clip.anchorReferenceId.empty()) {
        auto anchorOptions = getAnchorOptions(trackIndex, clip.clipId);
        for (const auto& option : anchorOptions) {
            if (option.clipReferenceId == clip.anchorReferenceId) {
                anchorLabel = option.label;
                break;
            }
        }
    }

    ImGui::SetNextItemWidth(-FLT_MIN);
    std::string comboId = std::format("##AnchorCombo{}", clip.clipId);
    if (ImGui::BeginCombo(comboId.c_str(), anchorLabel.c_str())) {
        // Track anchor option
        bool isTrackAnchor = clip.anchorReferenceId.empty();
        if (ImGui::Selectable("Track", isTrackAnchor)) {
            if (context.updateClip) {
                context.updateClip(trackIndex, clip.clipId, {}, clip.anchorOrigin, clip.position);
                changed = true;
            }
        }

        // Other clip anchors - show clip names
        auto anchorOptions = getAnchorOptions(trackIndex, clip.clipId);
        for (const auto& option : anchorOptions) {
            bool isSelected = (clip.anchorReferenceId == option.clipReferenceId);
            if (ImGui::Selectable(option.label.c_str(), isSelected)) {
                if (context.updateClip) {
                    context.updateClip(trackIndex, clip.clipId, option.clipReferenceId, clip.anchorOrigin, clip.position);
                    changed = true;
                }
            }
        }

        ImGui::EndCombo();
    }

    return changed;
}

bool SequenceEditor::renderOriginCombo(int32_t trackIndex, const ClipRow& clip, const RenderContext& context) {
    bool changed = false;
    ImGui::SetNextItemWidth(-FLT_MIN);
    std::string originComboId = std::format("##OriginCombo{}", clip.clipId);
    if (ImGui::BeginCombo(originComboId.c_str(), clip.anchorOrigin.c_str())) {
        bool isStart = (clip.anchorOrigin == "Start");
        if (ImGui::Selectable("Start", isStart)) {
            if (context.updateClip) {
                context.updateClip(trackIndex, clip.clipId, clip.anchorReferenceId, "Start", clip.position);
                changed = true;
            }
        }

        bool isEnd = (clip.anchorOrigin == "End");
        if (ImGui::Selectable("End", isEnd)) {
            if (context.updateClip) {
                context.updateClip(trackIndex, clip.clipId, clip.anchorReferenceId, "End", clip.position);
                changed = true;
            }
        }

        ImGui::EndCombo();
    }
    return changed;
}

bool SequenceEditor::renderPositionInput(int32_t trackIndex, const ClipRow& clip, const RenderContext& context) {
    bool changed = false;
    char posBuffer[64];
    strncpy(posBuffer, clip.position.c_str(), sizeof(posBuffer) - 1);
    posBuffer[sizeof(posBuffer) - 1] = '\0';

    ImGui::SetNextItemWidth(-FLT_MIN);
    std::string inputId = std::format("##PosInput{}", clip.clipId);
    if (ImGui::InputText(inputId.c_str(), posBuffer, sizeof(posBuffer), ImGuiInputTextFlags_EnterReturnsTrue)) {
        if (context.updateClip) {
            context.updateClip(trackIndex, clip.clipId, clip.anchorReferenceId, clip.anchorOrigin, std::string(posBuffer));
            changed = true;
        }
    }
    return changed;
}

bool SequenceEditor::renderNameInput(int32_t trackIndex, const ClipRow& clip, const RenderContext& context) {
    bool changed = false;
    static std::map<int32_t, std::array<char, 256>> nameBuffers;
    if (nameBuffers.find(clip.clipId) == nameBuffers.end()) {
        nameBuffers[clip.clipId] = {};
        strncpy(nameBuffers[clip.clipId].data(), clip.name.c_str(), nameBuffers[clip.clipId].size() - 1);
    }

    ImGui::SetNextItemWidth(-FLT_MIN);
    std::string nameInputId = std::format("##NameInput{}", clip.clipId);
    if (ImGui::InputText(nameInputId.c_str(), nameBuffers[clip.clipId].data(),
                        nameBuffers[clip.clipId].size(), ImGuiInputTextFlags_EnterReturnsTrue)) {
        if (context.updateClipName) {
            context.updateClipName(trackIndex, clip.clipId, std::string(nameBuffers[clip.clipId].data()));
            changed = true;
        }
    }
    return changed;
}

std::vector<SequenceEditor::AnchorOption> SequenceEditor::getAnchorOptions(int32_t trackIndex, int32_t currentClipId) const {
    std::vector<AnchorOption> options;
    for (const auto& [candidateTrackIndex, state] : windows_) {
        for (const auto& clip : state.displayClips) {
            if (clip.clipId <= 0)
                continue;
            if (candidateTrackIndex == trackIndex && clip.clipId == currentClipId)
                continue;

            AnchorOption option;
            option.trackIndex = candidateTrackIndex;
            option.clipReferenceId = clip.referenceId;
            const char* trackLabel = candidateTrackIndex == uapmd::kMasterTrackIndex ? "Master" : "Track";
            std::string clipLabel = clip.name.empty() ? std::format("Clip #{}", clip.clipId) : clip.name;
            option.label = candidateTrackIndex == uapmd::kMasterTrackIndex
                ? std::format("{}: {}", trackLabel, clipLabel)
                : std::format("{} {}: {}", trackLabel, candidateTrackIndex, clipLabel);
            options.push_back(std::move(option));
        }
    }

    std::sort(options.begin(), options.end(), [](const AnchorOption& a, const AnchorOption& b) {
        if (a.trackIndex != b.trackIndex)
            return a.trackIndex < b.trackIndex;
        return a.clipReferenceId < b.clipReferenceId;
    });
    return options;
}

void SequenceEditor::drawPlayheadIndicator(
    const SequenceEditorState& state,
    const RenderContext& context,
    float headerMinX,
    float headerMinY,
    float headerMaxX,
    float headerMaxY
) const {
    if (!state.timeline) {
        return;
    }

    auto& appModel = uapmd::AppModel::instance();
    const int32_t sampleRate = appModel.sampleRate();
    if (sampleRate <= 0) {
        return;
    }

    const auto& timelineState = appModel.timeline();
    const double playheadSeconds = timelineState.playheadPosition.toSeconds(sampleRate);
    if (!std::isfinite(playheadSeconds)) {
        return;
    }

    const int32_t maxFrame = state.timeline->GetMaxFrame();
    if (maxFrame <= 0) {
        return;
    }

    const double playheadUnits = secondsToUnits(context, playheadSeconds);

    const double clampedFrame = std::clamp(playheadUnits, 0.0, static_cast<double>(maxFrame));
    const double startFrame = static_cast<double>(state.timeline->GetStartTimestamp());
    if (clampedFrame < startFrame || clampedFrame > static_cast<double>(maxFrame)) {
        return;
    }

    const float scale = state.timeline->GetScale();
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

void SequenceEditor::pruneClipPreviewCache(SequenceEditorState& state) {
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

const uapmd::ClipData* SequenceEditor::findClipData(int32_t trackIndex, int32_t clipId) const {
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

std::string SequenceEditor::buildClipSignature(int32_t trackIndex, const ClipRow& clip, const uapmd::ClipData* clipData) const {
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
                       clip.timelineEnd - clip.timelineStart,
                       durationSamples,
                       sourceNodeId,
                       midiHash,
                       warpHash);
}

std::shared_ptr<ClipPreview> SequenceEditor::ensureClipPreview(
    int32_t trackIndex,
    const ClipRow& clip,
    SequenceEditorState& state
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

    const double fallbackDurationSeconds = std::max(
        0.0,
        static_cast<double>(clip.timelineEnd - clip.timelineStart)
    );

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
