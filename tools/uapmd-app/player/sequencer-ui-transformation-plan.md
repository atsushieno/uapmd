# Sequencer UI Transformation Plan - Phase 5

## Overview

This document describes the implementation plan for the Sequence Editor GUI - a per-track window for managing audio clips with hierarchical positioning (anchor support).

**Status:** Backend implementation complete (Phases 1-4). This phase adds the GUI layer.

## Current Problem

**Error:** "Could not add clip to track: Invalid track index"

**Root Cause:**
- `AppModel::app_tracks_` is initialized from `sequencer_.engine()->tracks()` at construction
- SequencerEngine starts with NO tracks - they're only created when plugins are instantiated
- `TransportController::loadFile()` tries to load clip into track 0, but `app_tracks_` is empty
- `AppModel::addClipToTrack()` validates `trackIndex < app_tracks_.size()` and fails

**Additional Issue:**
- `app_tracks_` is a stale copy - not updated when tracks are dynamically created via `addSimpleTrack()`
- Need to synchronize `app_tracks_` with sequencer tracks

## User Requirements

Based on user clarification, the Sequence Editor will have:

1. **Window Pattern:** Per-track window (like InstanceDetails - one window per track)
2. **Access Method:** "Sequence" button in TrackList for each track
3. **Track Creation:** Auto-create empty track when loading clip if no tracks exist
4. **Anchor Concept:** Clips can reference other clips as positioning anchors (hierarchical positioning)

## Sequence Editor Window Design

### Visual Layout

```
┌─────────────────────────────────────────────────┐
│ Sequence Editor - Track N                       │
├─────────────────────────────────────────────────┤
│ Clips:                                          │
│ ┌───────────────────────────────────────────┐  │
│ │ Anchor  │ Position  │ File      │ Delete │  │
│ ├─────────┼───────────┼───────────┼────────┤  │
│ │ Track   │ +0.000s   │ drum.wav  │   X    │  │
│ │ Clip #1 │ +2.500s   │ bass.wav  │   X    │  │
│ │ Track   │ +5.000s   │ lead.wav  │   X    │  │
│ └───────────────────────────────────────────┘  │
│                                                 │
│ [New Clip]  [Clear All]                        │
└─────────────────────────────────────────────────┘
```

### Clip List Table Columns

**Anchor Column:**
- Combo dropdown with options:
  - "Track" (default) - position is relative to timeline start (absolute)
  - Clip IDs of other clips on this track - position is relative to that clip's position
- Allows hierarchical positioning where clips can be anchored to other clips

**Position Column:**
- Text input for offset (supports positive/negative values)
- Format: `[+/-]seconds` or `[+/-]samples` or `[+/-]beats`
- Value is relative to the anchor point
- Example: If anchored to "Clip #1" at "+2.5s", final position = Clip #1 position + 2.5s

**Filename Column:**
- Display audio filename (basename)
- Show MIME type if available (e.g., "audio/wav", "audio/flac")
- Future: Click to browse/change file

**Delete Column:**
- Button to remove clip from track
- Confirmation prompt for safety

### Actions

**New Clip Button:**
- Opens file browser (portable-file-dialogs)
- Creates AudioFileReader from selected file
- Adds clip at timeline position 0 (or end of last clip)
- Default anchor: "Track" (absolute positioning)

**Clear All Button:**
- Shows confirmation dialog
- Removes all clips from the track
- Does not delete the track itself

## Implementation Steps

### Step 1: Fix Track Synchronization (Critical)

**Problem:** `app_tracks_` becomes stale when new tracks are created.

**Solution:** Add synchronization method

**File:** `tools/uapmd-app/AppModel.hpp`

Add declarations:
```cpp
void syncAppTracks();
void ensureDefaultTrack();
```

**File:** `tools/uapmd-app/AppModel.cpp`

Implement `syncAppTracks()`:
```cpp
void AppModel::syncAppTracks() {
    auto& uapmdTracks = sequencer_.engine()->tracks();

    // Remove deleted tracks from app_tracks_
    app_tracks_.erase(
        std::remove_if(app_tracks_.begin(), app_tracks_.end(),
            [&](const auto& appTrack) {
                return std::find(uapmdTracks.begin(), uapmdTracks.end(),
                                appTrack->uapmdTrack()) == uapmdTracks.end();
            }),
        app_tracks_.end()
    );

    // Add new tracks to app_tracks_
    for (size_t i = app_tracks_.size(); i < uapmdTracks.size(); ++i) {
        app_tracks_.push_back(
            std::make_unique<uapmd_app::AppTrack>(uapmdTracks[i], sample_rate_)
        );
    }
}
```

Call `syncAppTracks()` in:
- `createPluginInstanceAsync()` callback after track creation
- Any other path that creates tracks

### Step 2: Auto-Create Empty Track

**Goal:** When loading clip with no tracks, auto-create a default track.

**File:** `tools/uapmd-app/AppModel.cpp`

Modify `TransportController::loadFile()`:
```cpp
void TransportController::loadFile() {
    auto selection = pfd::open_file(...);
    if (selection.result().empty()) return;

    std::string filepath = selection.result()[0];
    auto reader = uapmd::createAudioFileReaderFromPath(filepath);
    if (!reader) {
        pfd::message("Load Failed", ...);
        return;
    }

    // NEW: Ensure at least one track exists
    if (appModel_->getAppTracks().empty()) {
        std::cout << "No tracks exist. Creating default track..." << std::endl;
        appModel_->ensureDefaultTrack();
    }

    // Existing clip loading code...
    // ...
}
```

Implement `ensureDefaultTrack()`:
```cpp
void AppModel::ensureDefaultTrack() {
    if (!app_tracks_.empty()) return;

    // Create empty track via sequencer
    // This creates a track without a plugin
    std::string dummyFormat = ""; // Empty track
    std::string dummyPluginId = "";

    sequencer_.engine()->addSimpleTrack(
        dummyFormat,
        dummyPluginId,
        [this](int32_t instanceId, int32_t trackIndex, std::string error) {
            if (!error.empty()) {
                std::cerr << "Failed to create default track: " << error << std::endl;
                return;
            }
            // Sync app_tracks with new sequencer track
            syncAppTracks();
            std::cout << "Created default track " << trackIndex << std::endl;
        }
    );
}
```

**Note:** May need to add `SequencerEngine::addEmptyTrack()` if `addSimpleTrack()` requires a plugin.

### Step 3: Extend ClipData for Anchor Support

**File:** `tools/uapmd-app/player/TimelineTypes.hpp`

Add anchor fields:
```cpp
struct ClipData {
    int32_t clipId{-1};
    TimelinePosition position;         // Absolute position on timeline
    int64_t durationSamples{0};
    int32_t sourceNodeInstanceId{-1};
    double gain{1.0};
    bool muted{false};

    // NEW: Anchor support
    int32_t anchorClipId{-1};         // -1 = track anchor (absolute), >= 0 = clip ID
    TimelinePosition anchorOffset;     // Offset from anchor position

    // Helper: Calculate absolute position from anchor
    TimelinePosition getAbsolutePosition(
        const std::unordered_map<int32_t, ClipData>& clips
    ) const {
        if (anchorClipId == -1) {
            return position;  // Already absolute (track anchor)
        }

        // Recursive anchor resolution
        auto it = clips.find(anchorClipId);
        if (it == clips.end()) {
            return position;  // Fallback if anchor not found
        }

        TimelinePosition anchorPos = it->second.getAbsolutePosition(clips);
        TimelinePosition result;
        result.samples = anchorPos.samples + anchorOffset.samples;
        result.beats = anchorPos.beats + anchorOffset.beats;
        return result;
    }

    // Helper: Get source position for playback
    int64_t getSourcePosition(
        const TimelinePosition& playhead,
        const std::unordered_map<int32_t, ClipData>& clips
    ) const {
        TimelinePosition absolutePos = getAbsolutePosition(clips);
        if (playhead.samples < absolutePos.samples) {
            return -1;  // Not started yet
        }
        int64_t elapsed = playhead.samples - absolutePos.samples;
        if (elapsed >= durationSamples) {
            return -1;  // Already ended
        }
        return elapsed;
    }
};
```

### Step 4: Update TrackClipManager for Anchors

**File:** `tools/uapmd-app/player/TrackClipManager.hpp`

Add method to get all clips (for anchor resolution):
```cpp
const std::unordered_map<int32_t, ClipData>& getClips() const;
```

**File:** `tools/uapmd-app/player/TrackClipManager.cpp`

Implement getter:
```cpp
const std::unordered_map<int32_t, ClipData>& TrackClipManager::getClips() const {
    return clips_;
}
```

Modify `getActiveClipsAt()` to use anchor resolution:
```cpp
std::vector<ClipData*> TrackClipManager::getActiveClipsAt(const TimelinePosition& position) {
    std::lock_guard lock(clips_mutex_);
    std::vector<ClipData*> activeClips;

    for (auto& [clipId, clip] : clips_) {
        if (clip.muted) continue;

        // Resolve absolute position using anchors
        TimelinePosition absolutePos = clip.getAbsolutePosition(clips_);

        // Check if clip is active at this position
        if (position.samples >= absolutePos.samples &&
            position.samples < absolutePos.samples + clip.durationSamples) {
            activeClips.push_back(&clip);
        }
    }

    return activeClips;
}
```

### Step 5: Create SequenceEditor Component

**New File:** `tools/uapmd-app/gui/SequenceEditor.hpp`

```cpp
#pragma once
#include "../player/TimelineTypes.hpp"
#include <imgui.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>

namespace uapmd {

class SequenceEditor {
public:
    struct ClipRow {
        int32_t clipId;
        int32_t anchorClipId;      // -1 = track anchor
        std::string position;       // Display string: "+2.5s"
        std::string filename;
        std::string mimeType;
    };

    struct SequenceEditorState {
        bool isOpen = false;
        std::vector<ClipRow> displayClips;
        int32_t selectedClipId = -1;

        // Edit state
        char positionEditBuffer[64] = {0};
        int selectedAnchorIndex = 0;
    };

    struct RenderContext {
        std::function<void(int32_t trackIndex)> refreshClips;
        std::function<void(int32_t trackIndex, const std::string& filepath)> addClip;
        std::function<void(int32_t trackIndex, int32_t clipId)> removeClip;
        std::function<void(int32_t trackIndex)> clearAllClips;
        std::function<void(int32_t trackIndex, int32_t clipId, int32_t anchorId, const std::string& position)> updateClip;
        float uiScale;
    };

    void showWindow(int32_t trackIndex);
    void hideWindow(int32_t trackIndex);
    void refreshClips(int32_t trackIndex);
    void render(RenderContext& context);

private:
    std::unordered_map<int32_t, SequenceEditorState> windows_;

    void renderWindow(int32_t trackIndex, SequenceEditorState& state, RenderContext& context);
    void renderClipTable(int32_t trackIndex, SequenceEditorState& state, RenderContext& context);
    void renderClipRow(const ClipRow& clip, int32_t trackIndex, SequenceEditorState& state, RenderContext& context);
    std::vector<std::string> getAnchorOptions(int32_t trackIndex, int32_t currentClipId);
};

} // namespace uapmd
```

**New File:** `tools/uapmd-app/gui/SequenceEditor.cpp`

```cpp
#include "SequenceEditor.hpp"
#include "../AppModel.hpp"
#include <format>
#include <filesystem>

namespace uapmd {

void SequenceEditor::showWindow(int32_t trackIndex) {
    windows_[trackIndex].isOpen = true;
    refreshClips(trackIndex);
}

void SequenceEditor::hideWindow(int32_t trackIndex) {
    windows_[trackIndex].isOpen = false;
}

void SequenceEditor::refreshClips(int32_t trackIndex) {
    auto& state = windows_[trackIndex];
    state.displayClips.clear();

    auto tracks = AppModel::instance().getAppTracks();
    if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(tracks.size())) {
        return;
    }

    auto& clipManager = tracks[trackIndex]->clipManager();
    auto& clips = clipManager.getClips();

    for (const auto& [clipId, clip] : clips) {
        ClipRow row;
        row.clipId = clipId;
        row.anchorClipId = clip.anchorClipId;

        // Format position
        double seconds = static_cast<double>(clip.anchorOffset.samples) / AppModel::instance().sampleRate();
        row.position = std::format("{:+.3f}s", seconds);

        // Get filename from source node
        // TODO: Need way to get filename from AppAudioFileSourceNode
        row.filename = "audio_file.wav";  // Placeholder
        row.mimeType = "";

        state.displayClips.push_back(row);
    }
}

void SequenceEditor::render(RenderContext& context) {
    // Render all open windows
    for (auto& [trackIndex, state] : windows_) {
        if (state.isOpen) {
            renderWindow(trackIndex, state, context);
        }
    }
}

void SequenceEditor::renderWindow(int32_t trackIndex, SequenceEditorState& state, RenderContext& context) {
    std::string title = std::format("Sequence Editor - Track {}", trackIndex);

    ImGui::SetNextWindowSize(ImVec2(600 * context.uiScale, 400 * context.uiScale), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin(title.c_str(), &state.isOpen)) {
        ImGui::End();
        return;
    }

    ImGui::Text("Clips:");
    ImGui::Separator();

    // Clip table
    renderClipTable(trackIndex, state, context);

    ImGui::Separator();

    // Action buttons
    if (ImGui::Button("New Clip")) {
        // TODO: Open file dialog via context callback
        context.addClip(trackIndex, "");
    }

    ImGui::SameLine();

    if (ImGui::Button("Clear All")) {
        // TODO: Confirmation dialog
        context.clearAllClips(trackIndex);
    }

    ImGui::End();
}

void SequenceEditor::renderClipTable(int32_t trackIndex, SequenceEditorState& state, RenderContext& context) {
    if (ImGui::BeginTable("ClipTable", 4,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_ScrollY,
                          ImVec2(0, 250 * context.uiScale))) {

        // Setup columns
        ImGui::TableSetupColumn("Anchor", ImGuiTableColumnFlags_WidthFixed, 100 * context.uiScale);
        ImGui::TableSetupColumn("Position", ImGuiTableColumnFlags_WidthFixed, 100 * context.uiScale);
        ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Delete", ImGuiTableColumnFlags_WidthFixed, 60 * context.uiScale);
        ImGui::TableHeadersRow();

        // Render rows
        for (const auto& clip : state.displayClips) {
            renderClipRow(clip, trackIndex, state, context);
        }

        ImGui::EndTable();
    }
}

void SequenceEditor::renderClipRow(const ClipRow& clip, int32_t trackIndex, SequenceEditorState& state, RenderContext& context) {
    ImGui::TableNextRow();

    // Anchor column
    ImGui::TableSetColumnIndex(0);
    auto anchorOptions = getAnchorOptions(trackIndex, clip.clipId);
    std::string anchorLabel = clip.anchorClipId == -1 ? "Track" : std::format("Clip #{}", clip.anchorClipId);
    if (ImGui::BeginCombo(std::format("##anchor{}", clip.clipId).c_str(), anchorLabel.c_str())) {
        // TODO: Implement anchor selection
        ImGui::EndCombo();
    }

    // Position column
    ImGui::TableSetColumnIndex(1);
    ImGui::Text("%s", clip.position.c_str());
    // TODO: Make editable with InputText

    // Filename column
    ImGui::TableSetColumnIndex(2);
    ImGui::Text("%s", clip.filename.c_str());
    if (!clip.mimeType.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", clip.mimeType.c_str());
    }

    // Delete column
    ImGui::TableSetColumnIndex(3);
    if (ImGui::Button(std::format("X##{}", clip.clipId).c_str())) {
        context.removeClip(trackIndex, clip.clipId);
        context.refreshClips(trackIndex);
    }
}

std::vector<std::string> SequenceEditor::getAnchorOptions(int32_t trackIndex, int32_t currentClipId) {
    std::vector<std::string> options;
    options.push_back("Track");

    auto tracks = AppModel::instance().getAppTracks();
    if (trackIndex >= 0 && trackIndex < static_cast<int32_t>(tracks.size())) {
        auto& clips = tracks[trackIndex]->clipManager().getClips();
        for (const auto& [clipId, clip] : clips) {
            if (clipId != currentClipId) {
                options.push_back(std::format("Clip #{}", clipId));
            }
        }
    }

    return options;
}

} // namespace uapmd
```

### Step 6: Add Sequence Button to TrackList

**File:** `tools/uapmd-app/gui/TrackList.hpp`

Add callback member:
```cpp
std::function<void(int32_t trackIndex)> onShowSequence;
```

**File:** `tools/uapmd-app/gui/TrackList.cpp`

In table setup, add column:
```cpp
ImGui::TableSetupColumn("Sequence", ImGuiTableColumnFlags_WidthFixed, 80.0f);
```

In row rendering, add button:
```cpp
// After Details button
ImGui::TableSetColumnIndex(5);  // Adjust index
if (ImGui::Button(std::format("Sequence##{}", instance.id).c_str())) {
    if (onShowSequence) {
        onShowSequence(instance.trackIndex);
    }
}
```

### Step 7: Integrate into MainWindow

**File:** `tools/uapmd-app/gui/MainWindow.hpp`

Add member:
```cpp
#include "SequenceEditor.hpp"

class MainWindow {
    // ... existing members ...
    SequenceEditor sequenceEditor_;
};
```

**File:** `tools/uapmd-app/gui/MainWindow.cpp`

In `render()`, add SequenceEditor rendering:
```cpp
// After InstanceDetails rendering
SequenceEditor::RenderContext seqContext{
    .refreshClips = [this](int32_t trackIndex) {
        sequenceEditor_.refreshClips(trackIndex);
    },
    .addClip = [this](int32_t trackIndex, const std::string& filepath) {
        // TODO: Implement file dialog and clip creation
        auto reader = uapmd::createAudioFileReaderFromPath(filepath);
        if (reader) {
            uapmd_app::TimelinePosition pos;
            pos.samples = 0;
            AppModel::instance().addClipToTrack(trackIndex, pos, std::move(reader));
            sequenceEditor_.refreshClips(trackIndex);
        }
    },
    .removeClip = [this](int32_t trackIndex, int32_t clipId) {
        AppModel::instance().removeClipFromTrack(trackIndex, clipId);
        sequenceEditor_.refreshClips(trackIndex);
    },
    .clearAllClips = [this](int32_t trackIndex) {
        // Remove all clips from track
        auto tracks = AppModel::instance().getAppTracks();
        if (trackIndex >= 0 && trackIndex < static_cast<int32_t>(tracks.size())) {
            auto& clips = tracks[trackIndex]->clipManager().getClips();
            std::vector<int32_t> clipIds;
            for (const auto& [clipId, clip] : clips) {
                clipIds.push_back(clipId);
            }
            for (int32_t clipId : clipIds) {
                AppModel::instance().removeClipFromTrack(trackIndex, clipId);
            }
            sequenceEditor_.refreshClips(trackIndex);
        }
    },
    .updateClip = [this](int32_t trackIndex, int32_t clipId, int32_t anchorId, const std::string& position) {
        // TODO: Parse position string and update clip
    },
    .uiScale = uiScale_,
};
sequenceEditor_.render(seqContext);
```

Set TrackList callback:
```cpp
trackList_.onShowSequence = [this](int32_t trackIndex) {
    sequenceEditor_.showWindow(trackIndex);
};
```

### Step 8: Update CMakeLists.txt

**File:** `tools/uapmd-app/CMakeLists.txt`

Add to `UAPMD_APP_SOURCE_FILES`:
```cmake
gui/SequenceEditor.cpp
```

## Critical Files Summary

### Files to Create
1. `tools/uapmd-app/gui/SequenceEditor.hpp` - Component header
2. `tools/uapmd-app/gui/SequenceEditor.cpp` - Component implementation

### Files to Modify
3. `tools/uapmd-app/player/TimelineTypes.hpp` - Add anchor fields to ClipData
4. `tools/uapmd-app/player/TrackClipManager.hpp` - Add getClips() method
5. `tools/uapmd-app/player/TrackClipManager.cpp` - Implement anchor resolution
6. `tools/uapmd-app/AppModel.hpp` - Add syncAppTracks(), ensureDefaultTrack()
7. `tools/uapmd-app/AppModel.cpp` - Implement synchronization and auto-track
8. `tools/uapmd-app/gui/MainWindow.hpp` - Add SequenceEditor member
9. `tools/uapmd-app/gui/MainWindow.cpp` - Integrate SequenceEditor rendering
10. `tools/uapmd-app/gui/TrackList.hpp` - Add onShowSequence callback
11. `tools/uapmd-app/gui/TrackList.cpp` - Add Sequence button column
12. `tools/uapmd-app/CMakeLists.txt` - Add SequenceEditor.cpp to build

## Testing Checklist

- [ ] Tracks stay synchronized when plugins are created/removed
- [ ] Loading clip with no tracks auto-creates default track
- [ ] "Sequence" button appears in TrackList for each track
- [ ] Clicking "Sequence" opens per-track window
- [ ] Clip table displays anchor, position, filename, delete button
- [ ] "New Clip" button opens file dialog and adds clip
- [ ] "Clear All" button removes all clips with confirmation
- [ ] Anchor dropdown shows "Track" + other clips
- [ ] Changing anchor updates clip positioning
- [ ] Position editing updates clip offset
- [ ] Delete button removes individual clips
- [ ] Clips play back correctly with anchor offsets resolved
- [ ] No "Invalid track index" errors

## Future Enhancements

- Click filename to change audio file
- Drag-and-drop clip reordering
- Visual timeline with clips as horizontal bars
- Clip duplication
- Bulk clip operations (select multiple)
- Undo/redo support
- Clip color coding
- Waveform preview in clip list
