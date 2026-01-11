# Plan: Transform Audio Player to Sequencer with Audio Source Nodes

## Overview

Transform the current single-file audio player into a comprehensive DAW-like sequencer by:
1. Building **app-level audio source node layer** on top of existing uapmd audio graph (unchanged)
2. Moving audio file playback from global SequencerEngine into **track-owned clip playback at app level**
3. Making device input (microphone) an **app-level source node** assigned to specific tracks
4. **Mapping** app-level nodes to uapmd AudioPluginNode via adapter pattern
5. Preparing architecture for **timeline features**: multiple clips per track with basic playback

**Key Architecture Decision:** Keep uapmd audio graph completely unchanged. Build new source node layer in uapmd-app that sits on top.

---

## Current State Analysis

### Existing Audio Player Architecture

**Single File Playback:**
- `SequencerEngine` loads entire audio file into memory (`audio_file_buffer_`)
- Lines 603-645 in `src/uapmd/Sequencer/SequencerEngine.cpp`: File loading/unloading
- Lines 250-261: Audio file mixed additively with device input in `processAudio()`
- Mixed input sent to **ALL tracks globally**

**Transport Control:**
- `TransportController` in `tools/uapmd-app/AppModel.hpp` (lines 17-50)
- Simple play/pause/stop, no loop points or timeline features
- `loadFile()`/`unloadFile()` manages single audio file

**Limitations:**
- No timeline/clip concept - just single file playback
- Audio sources external to graph (not nodes)
- Device input and audio file globally merged before track processing

### Current Graph Architecture

**Linear Plugin Chain:**
- `AudioPluginGraph`: Sequential processing of plugin nodes
- `AudioPluginNode`: Wraps VST3/CLAP/LV2/AU instances
- `AudioPluginTrack`: Contains graph + MIDI event routing
- No audio source node types - only effect/instrument plugins

**Processing Flow (SequencerEngine.cpp:199-401):**
1. Load audio file into `merged_input_`
2. Add device input to `merged_input_`
3. Copy `merged_input_` to ALL tracks
4. Process each track's plugin chain
5. Mix all track outputs

**Key Structures:**
- `AudioProcessContext` (remidy/priv/processing-context.hpp): Audio buffer + event context
- `TrackContext`: Tempo, playback position per track
- `MasterContext`: Global tempo, sample rate, playback state

---

## User Requirements (Clarified)

✅ **Each track has its own source nodes** (like DAW clips on tracks)
✅ **Device input becomes a source node** (assigned to specific tracks)
✅ **Timeline features to prepare for:**
- Multiple clips per track arranged on timeline
- Basic clip playback (full file, no regions/looping in this phase)

✅ **Complete replacement** (no backward compatibility needed)

---

## Proposed Architecture

### 1. App-Level Audio Source Node Layer

**IMPORTANT: uapmd audio graph stays completely unchanged!**

**New App-Level Node Hierarchy (tools/uapmd-app/player/):**
```
AppAudioNode (new app-level base interface)
├── AppPluginNode (wraps uapmd::AudioPluginNode)
├── AppSourceNode (new abstract base for source nodes)
│   ├── AppAudioFileSourceNode (plays audio file clips)
│   ├── AppDeviceInputSourceNode (captures device input)
│   └── [Future: AppGeneratorSourceNode]
```

**Key Design Principles:**
1. **App layer only**: All source node logic lives in tools/uapmd-app/
2. **Adapter pattern**: AppPluginNode wraps existing uapmd::AudioPluginNode
3. **No core changes**: uapmd AudioPluginNode, AudioPluginGraph, AudioPluginTrack unchanged
4. **Mapping at runtime**: App layer manages source nodes, feeds audio into uapmd graph via input buffers

### 2. Timeline and Clip Architecture

**Keep Timeline Structures in App Layer:**
- Timeline/clip structures stay in `tools/uapmd-app/player/`
- No changes to core uapmd engine
- App layer manages all timeline state and clip playback

**App Types (new file: `tools/uapmd-app/player/TimelineTypes.hpp`):**
```cpp
struct TimelinePosition {
    int64_t samples;           // Primary (RT-safe)
    double beats;              // Secondary (for UI/musical time)
    // Conversion helpers
};

struct ClipData {
    int32_t clipId;
    TimelinePosition position;      // Position on timeline
    int64_t durationSamples;        // Duration of clip (full file length)
    int32_t sourceNodeInstanceId;   // Which source node plays this

    // Playback properties
    double gain;
    bool muted;

    // Note: Clip regions, looping, automation, and time-stretch NOT included in this phase
    // Each clip plays the entire audio file from start to finish
};

struct TimelineState {
    TimelinePosition playheadPosition;
    bool isPlaying;
    bool loopEnabled;
    TimelinePosition loopStart, loopEnd;
    double tempo;
    int32_t timeSignatureNumerator, timeSignatureDenominator;
};
```

**Clip Manager Per Track (new: `tools/uapmd-app/player/TrackClipManager.hpp`):**
- App-level component that manages clips for a single app track
- CRUD operations: add/remove/move/resize/split clips
- Query active clips at timeline position
- Thread-safe: mutex-protected (UI thread), cached queries (RT thread)

### 3. Processing Flow Changes

**Old Flow (in AppModel):**
```
AppModel processes audio callback:
  1. SequencerEngine loads audio file globally
  2. SequencerEngine::processAudio() mixes file + device input
  3. Merged input sent to all uapmd tracks
  4. Each track processes its plugin chain
```

**New Flow (in AppModel):**
```
AppModel processes audio callback:
  1. Update app-level timeline state
  2. For each app track:
      a. Query active clips at timeline position
      b. Process app-level source nodes for clips
      c. Mix source outputs into buffer
      d. Set buffer as input to uapmd::AudioPluginTrack
      e. Call uapmd track->processAudio() (unchanged!)
  3. Mix all uapmd track outputs (existing code)
```

**Key Point:** uapmd::SequencerEngine and uapmd::AudioPluginTrack stay exactly as-is. App layer handles source mixing before calling into core.

### 4. App Track Layer

**NO CHANGES to uapmd::AudioPluginTrack!**

**New App Track Wrapper (tools/uapmd-app/player/AppTrack.hpp):**
```cpp
class AppTrack {
public:
    AppTrack(uapmd::AudioPluginTrack* uapmdTrack);

    // App-level clip management
    TrackClipManager& clipManager();
    int32_t addClip(ClipData clip, std::unique_ptr<AppAudioFileSourceNode> sourceNode);
    bool removeClip(int32_t clipId);

    // App-level source node management
    bool addDeviceInputSource(std::unique_ptr<AppDeviceInputSourceNode> sourceNode);
    bool removeSource(int32_t sourceId);

    // Timeline-aware processing (NEW)
    void processAudioWithTimeline(
        const TimelineState& timeline,
        float** deviceInputBuffers,
        int32_t frameCount
    );

private:
    uapmd::AudioPluginTrack* uapmd_track_;  // Wrapped uapmd track (unchanged)
    TrackClipManager clip_manager_;
    std::vector<std::unique_ptr<AppSourceNode>> source_nodes_;
    std::vector<float> mixed_source_buffer_;  // Temporary buffer for mixing sources
};
```

**App Track Processing:**
1. Query active clips from clip_manager_
2. Process each clip's source node into temp buffers
3. Mix all source outputs into mixed_source_buffer_
4. Copy mixed_source_buffer_ into uapmd track's input buffer
5. Call `uapmd_track_->processAudio()` (unchanged core API!)

### 5. SequencerEngine API Cleanup

**Changes to uapmd::SequencerEngine (remove audio file support):**

**REMOVE from SequencerEngine:**
- `audio_file_buffer_`, `audio_file_reader_`, `audio_file_read_position_`, `audio_file_mutex_`
- `merged_input_` buffer (if used for audio file mixing)
- `loadAudioFile()`, `unloadAudioFile()`, `audioFileDurationSeconds()` methods
- Audio file mixing logic in `processAudio()` (lines 216-291 in SequencerEngine.cpp)

**Files to modify:**
- `include/uapmd/priv/sequencer/SequencerEngine.hpp` - Remove audio file API
- `src/uapmd/Sequencer/SequencerEngine.cpp` - Remove implementation (lines 602-645, lines 216-291)

**Rationale:** Audio file playback moving to app layer, so core engine no longer needs this functionality.

### 6. AppModel Restructuring

**Changes in AppModel (tools/uapmd-app/AppModel.hpp/cpp):**

**REMOVE from AppModel:**
- Audio file loading code that calls `sequencer_->loadAudioFile()`
- Direct calls to `sequencer_->startPlayback()` etc.

**ADD to AppModel:**
- `TimelineState timeline_` member (app-level)
- `std::vector<std::unique_ptr<AppTrack>> app_tracks_` (wraps uapmd tracks)
- Clip management: `addClipToTrack()`, `removeClipFromTrack()`
- Device input routing: `addDeviceInputToTrack()`

**Modified Audio Callback in AppModel:**
```cpp
void AppModel::processAudioCallback(...) {
    // Update timeline state
    if (timeline_.isPlaying) {
        timeline_.playheadPosition.samples += frameCount;
        // Handle loop region
    }

    // Process each app track
    for (auto& appTrack : app_tracks_) {
        appTrack->processAudioWithTimeline(
            timeline_,
            deviceInputBuffers,
            frameCount
        );
        // AppTrack internally calls uapmd_track_->processAudio()
    }

    // Rest of existing code unchanged
    // (spectrum analysis, output mixing handled by SequencerEngine)
}
```

### 7. API Changes

**uapmd::SequencerEngine API:**
```cpp
// REMOVE:
void loadAudioFile(std::unique_ptr<AudioFileReader>);
void unloadAudioFile();
double audioFileDurationSeconds() const;

// No additions - rest of API unchanged
```

**AppModel API (tools/uapmd-app/AppModel.hpp):**
```cpp
// REMOVE (in TransportController):
void loadFile();
void unloadFile();

// ADD to AppModel:
TimelineState& timeline();

// Clip management (NEW)
int32_t addClipToTrack(
    int32_t trackIndex,
    TimelinePosition position,
    std::unique_ptr<AudioFileReader> reader
);
bool removeClipFromTrack(int32_t trackIndex, int32_t clipId);

// Device input routing (NEW)
int32_t addDeviceInputToTrack(
    int32_t trackIndex,
    const std::vector<uint32_t>& channelIndices
);
```

**TransportController API:**
```cpp
// REMOVE:
void loadFile();
void unloadFile();
std::string currentFile() const;
double playbackLength() const;

// ADD:
void seek(double timeInSeconds);
void setLoopRegion(double startSeconds, double endSeconds);
void enableLoop(bool enabled);
void setTempo(double bpm);
double tempo() const;
bool loopEnabled() const;
```

---

## Critical Files

### New Files to Create (ALL in tools/uapmd-app/)

**App-Level Player Components:**
1. `tools/uapmd-app/player/TimelineTypes.hpp` - Timeline data structures (TimelinePosition, ClipData, TimelineState)
2. `tools/uapmd-app/player/AppAudioNode.hpp` - Base interface for app-level nodes
3. `tools/uapmd-app/player/AppSourceNode.hpp` - Abstract source node base
4. `tools/uapmd-app/player/AppAudioFileSourceNode.hpp` - Audio file clip playback node
5. `tools/uapmd-app/player/AppAudioFileSourceNode.cpp` - Implementation
6. `tools/uapmd-app/player/AppDeviceInputSourceNode.hpp` - Device input capture node
7. `tools/uapmd-app/player/AppDeviceInputSourceNode.cpp` - Implementation
8. `tools/uapmd-app/player/AppPluginNode.hpp` - Adapter wrapping uapmd::AudioPluginNode
9. `tools/uapmd-app/player/TrackClipManager.hpp` - Per-track clip management
10. `tools/uapmd-app/player/TrackClipManager.cpp` - Implementation
11. `tools/uapmd-app/player/AppTrack.hpp` - App-level track wrapper
12. `tools/uapmd-app/player/AppTrack.cpp` - Implementation with timeline-aware processing

### Files to Modify

**Core Engine (minimal changes - remove audio file support only):**
13. `include/uapmd/priv/sequencer/SequencerEngine.hpp` - Remove `loadAudioFile()`, `unloadAudioFile()`, `audioFileDurationSeconds()`
14. `src/uapmd/Sequencer/SequencerEngine.cpp` - Remove audio file members, loading/unloading impl, mixing logic

**App Layer:**
15. `tools/uapmd-app/AppModel.hpp` - Add timeline state, app tracks, clip management APIs
16. `tools/uapmd-app/AppModel.cpp` - Implement audio callback with timeline processing, remove loadFile/unloadFile
17. `tools/uapmd-app/gui/MainWindow.cpp` - Update UI to use new clip-based APIs

---

## Implementation Phases

**IMPORTANT: All work happens in tools/uapmd-app/. No changes to core uapmd!**

### Phase 1: App-Level Node Architecture
**Goal:** Create app-level node abstractions without touching core

1. Create `tools/uapmd-app/player/AppAudioNode.hpp` - Base interface for app nodes
2. Create `tools/uapmd-app/player/AppSourceNode.hpp` - Abstract source node base with `seek()`, `processAudio()`, etc.
3. Create `tools/uapmd-app/player/AppPluginNode.hpp` - Simple adapter wrapping `uapmd::AudioPluginNode*`
4. Implement `AppAudioFileSourceNode`:
   - Takes `std::unique_ptr<AudioFileReader>`
   - Loads entire file into memory (matching current behavior)
   - `processAudio()` generates audio into buffer at current playback position
5. Implement `AppDeviceInputSourceNode`:
   - Captures device input channels
   - Passes through device audio in `processAudio()`

**Success Criteria:** Source nodes can generate audio into buffers, existing uapmd unchanged

### Phase 2: Timeline and Clip Management
**Goal:** Add app-level timeline structures and per-track clip management

1. Create `tools/uapmd-app/player/TimelineTypes.hpp`:
   - `TimelinePosition`, `ClipData`, `TimelineState` structs
   - Time conversion helpers (samples ↔ seconds)
2. Create `tools/uapmd-app/player/TrackClipManager`:
   - CRUD operations for clips
   - Query active clips at timeline position
   - Thread-safe (mutex for UI thread, cached queries for RT)
3. Create `tools/uapmd-app/player/AppTrack`:
   - Wraps `uapmd::AudioPluginTrack*` (doesn't own it)
   - Contains `TrackClipManager`
   - Contains `std::vector<std::unique_ptr<AppSourceNode>>` for source nodes
   - Implements `processAudioWithTimeline()`:
     a. Query active clips
     b. Process source nodes for clips
     c. Mix into temp buffer
     d. Copy to uapmd track input buffer
     e. Call `uapmd_track_->processAudio()`

**Success Criteria:** App tracks can manage clips and process timeline-aware audio

### Phase 3: SequencerEngine Cleanup
**Goal:** Remove audio file support from core engine

1. Remove from `SequencerEngine.hpp`:
   - `loadAudioFile()`, `unloadAudioFile()`, `audioFileDurationSeconds()` declarations
2. Remove from `SequencerEngine.cpp`:
   - Member variables: `audio_file_buffer_`, `audio_file_reader_`, `audio_file_read_position_`, `audio_file_mutex_`
   - Lines 602-645: `loadAudioFile()`/`unloadAudioFile()` implementations
   - Lines 216-291: Audio file mixing logic in `processAudio()` (if applicable)
   - Keep device input handling and track processing unchanged

**Success Criteria:** SequencerEngine compiles without audio file support, existing plugin playback still works

### Phase 4: AppModel Integration
**Goal:** Move timeline/playback control to AppModel

1. Add to `AppModel`:
   - `TimelineState timeline_` member
   - `std::vector<std::unique_ptr<AppTrack>> app_tracks_`
   - Initialize app_tracks_ to wrap existing uapmd tracks from sequencer_
2. Add clip management APIs to AppModel:
   - `addClipToTrack(trackIndex, position, reader)`
   - `removeClipFromTrack(trackIndex, clipId)`
   - `addDeviceInputToTrack(trackIndex, channelIndices)`
3. Modify AppModel audio callback:
   - Update timeline state (playhead, loop handling)
   - Call `app_track->processAudioWithTimeline()` for each track
   - SequencerEngine still handles final output mixing and spectrum analysis
4. Remove from AppModel/TransportController:
   - `loadFile()`, `unloadFile()` methods
   - Calls to `sequencer_->loadAudioFile()`

**Success Criteria:** Audio playback works via app-level clips, old file loading removed

### Phase 5: GUI Updates
**Goal:** Update UI to use new clip-based APIs

1. Update `TransportController` in `AppModel.hpp`:
   - Remove `loadFile()`, `unloadFile()`, `currentFile()`, `playbackLength()`
   - Add `seek()`, `setLoopRegion()`, `enableLoop()`, `setTempo()`
2. Update `MainWindow.cpp` (lines 675-799):
   - Replace Load/Unload buttons with "Add Clip to Track" UI
   - Add timeline scrubbing support (enable seeking on position slider)
   - Add tempo/loop controls
   - Update time display to use timeline state
3. Test with existing plugins to ensure backward compatibility

**Success Criteria:** GUI works with new clip APIs, can add multiple clips to tracks

### Phase 6: Advanced Features (Future)
**Not in initial scope:**
- Streaming playback for large audio files
- Crossfades between clips
- Multiple clips per track with overlap mixing
- Clip regions (start/end points within source file)
- Clip looping

**Explicitly excluded (out of scope):**
- Time-stretch/pitch-shift support
- Automation curves

---

## Design Decisions and Trade-offs

### Decision 1: App-Level Source Nodes (NOT Core Graph Nodes)
**Chosen:** Source nodes live in app layer, NOT integrated into core uapmd graph

**Rationale:**
- Avoids invasive core changes
- Keeps uapmd stable and backward compatible
- Easier to test and iterate in app layer
- Can later migrate to core if needed

**Trade-off:** Some duplication between app nodes and core nodes, but worth it for safety

### Decision 2: Timeline Position Representation
**Chosen:** Primary = samples (int64_t), secondary = musical time (beats)

**Rationale:** RT-safe (no floating point division), audio processing in sample domain
**Trade-off:** Tempo changes require recalculating positions; convert to beats for UI display

### Decision 3: In-Memory vs Streaming Playback
**Phase 1-5:** In-memory (current behavior)
**Phase 6:** Streaming (production-ready for large files)

**Rationale:** Faster migration, maintain current memory model
**Trade-off:** Memory usage for large projects until Phase 6

### Decision 4: Thread Safety
**Clip Manager:** Mutex-protected (UI thread)
**Timeline State:** Atomic variables (RT thread)
**Active Clip Queries:** Cached results (no RT allocations)

**Rationale:** Separate UI modifications from RT audio processing
**Trade-off:** Lock contention if UI modifies clips during playback (mitigated by deferred updates)

---

## Migration Notes

**NO BREAKING CHANGES to core uapmd!**

**App-Level Changes:**
- `TransportController::loadFile()` removed → Use `AppModel::addClipToTrack()`
- Audio file playback moved from SequencerEngine to AppModel/AppTrack layer
- uapmd::SequencerEngine still handles global playback state via existing API

**Migration Example:**
```cpp
// OLD (AppModel.cpp):
sequencer_->loadAudioFile(std::move(reader));
transport.play();

// NEW (AppModel.cpp):
app_model_->addClipToTrack(
    0,  // track index
    TimelinePosition::fromSamples(0, sampleRate),
    std::move(reader)  // Plays entire file
);
transport.play();
```

**Code Removal Checklist:**

**SequencerEngine (core):**
- `SequencerEngine.hpp` lines 78-81: Audio file API declarations
- `SequencerEngine.cpp` lines 23-27: Audio file member variables
- `SequencerEngine.cpp` lines 602-645: `loadAudioFile()`/`unloadAudioFile()` implementations
- `SequencerEngine.cpp` lines 216-291: Audio file mixing in `processAudio()` (if applicable)

**AppModel (app layer):**
- `AppModel.cpp`: Remove `loadFile()` / `unloadFile()` implementations
- `AppModel.cpp`: Remove calls to `sequencer_->loadAudioFile()`
- `AppModel.hpp`: Remove `loadFile()` / `unloadFile()` in TransportController
- `MainWindow.cpp`: Update Load/Unload button handlers

---

## Implementation Order Recommendation

**Start with:**
1. TimelineTypes.hpp (app-level data structures)
2. AppAudioNode.hpp + AppSourceNode.hpp (app-level node interfaces)
3. AppAudioFileSourceNode (basic audio file playback)
4. TrackClipManager (clip CRUD + queries)
5. AppTrack (wraps uapmd track, timeline processing)
6. AppModel integration (use AppTrack in audio callback)
7. GUI updates

This bottom-up order ensures dependencies are built incrementally with testing at each layer, **without ever modifying core uapmd**.
