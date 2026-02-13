#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <imgui.h>
#include <uapmd/uapmd.hpp>
#include "SequenceEditor.hpp"
#include "MidiDumpWindow.hpp"
#include "PluginSelector.hpp"
#include "InstanceDetails.hpp"
#include "../AppModel.hpp"

namespace uapmd::gui {

// Callbacks to parent window for plugin-related UI operations
struct TimelineEditorCallbacks {
    std::function<std::optional<TrackInstance>(int32_t instanceId)> buildTrackInstanceInfo;
    std::function<void(int32_t instanceId)> handleRemoveInstance;
    std::function<void(int32_t instanceId)> savePluginState;
    std::function<void(int32_t instanceId)> loadPluginState;
    std::function<void(int32_t instanceId)> onInstanceDetailsClosed;
};

class TimelineEditor {
public:
    TimelineEditor();

    // Setup callbacks to parent window
    void setCallbacks(TimelineEditorCallbacks callbacks);

    // Main rendering
    void render(float uiScale);
    void update();

    // UI component accessors
    SequenceEditor& sequenceEditor() { return sequenceEditor_; }
    MidiDumpWindow& midiDumpWindow() { return midiDumpWindow_; }
    PluginSelector& pluginSelector() { return pluginSelector_; }
    InstanceDetails& instanceDetails() { return instanceDetails_; }

    // Plugin selector window
    bool& showPluginSelectorWindow() { return showPluginSelectorWindow_; }
    void renderPluginSelectorWindow(float uiScale);

    // Timeline unit conversion
    double secondsToTimelineUnits(double seconds) const;
    double timelineUnitsToSeconds(double units) const;
    const char* timelineUnitsLabel() const { return timelineUnitsLabel_.c_str(); }
    void invalidateMasterTrackSnapshot();

    // Clip management
    void refreshSequenceEditorForTrack(int32_t trackIndex);
    void refreshAllSequenceEditorTracks();
    void addClipToTrack(int32_t trackIndex, const std::string& filepath);
    void addClipToTrackAtPosition(int32_t trackIndex, const std::string& filepath, double positionSeconds);
    void removeClipFromTrack(int32_t trackIndex, int32_t clipId);
    void clearAllClipsFromTrack(int32_t trackIndex);
    void updateClip(int32_t trackIndex, int32_t clipId, int32_t anchorId, const std::string& origin, const std::string& position);
    void updateClipName(int32_t trackIndex, int32_t clipId, const std::string& name);
    void changeClipFile(int32_t trackIndex, int32_t clipId);
    void moveClipAbsolute(int32_t trackIndex, int32_t clipId, double seconds);

    // MIDI dump
    void showMidiClipDump(int32_t trackIndex, int32_t clipId);
    void showMasterMetaDump();

    // Import
    void importSmfTracks();

    // Track layout change handler
    void handleTrackLayoutChange(const uapmd::AppModel::TrackLayoutChange& change);

    // Child window size management (forwarded from MainWindow)
    void setChildWindowSizeHelper(std::function<void(const std::string&, ImVec2)> setSize,
                                   std::function<void(const std::string&)> updateSize);

private:
    SequenceEditor sequenceEditor_;
    MidiDumpWindow midiDumpWindow_;
    PluginSelector pluginSelector_;
    InstanceDetails instanceDetails_;

    bool showPluginSelectorWindow_ = false;

    // Master track snapshot and signature for change detection
    std::shared_ptr<uapmd::AppModel::MasterTrackSnapshot> masterTrackSnapshot_;
    std::string masterTrackSignature_;

    // Tempo segments for timeline unit conversion
    struct TempoSegment {
        double startTime{0.0};
        double endTime{0.0};
        double bpm{120.0};
        double accumulatedBeats{0.0};
    };
    std::vector<TempoSegment> tempoSegments_;
    std::string timelineUnitsLabel_ = "seconds";

    // Callbacks to parent window
    TimelineEditorCallbacks callbacks_;

    // Child window size helpers
    std::function<void(const std::string&, ImVec2)> setNextChildWindowSize_;
    std::function<void(const std::string&)> updateChildWindowSizeState_;

    // Internal rendering
    void renderTrackList(const SequenceEditor::RenderContext& context);
    void renderMasterTrackRow(const SequenceEditor::RenderContext& context);
    void renderTrackRow(int32_t trackIndex, const SequenceEditor::RenderContext& context);
    void deleteTrack(int32_t trackIndex);

    // Tempo management
    void rebuildTempoSegments(const std::shared_ptr<uapmd::AppModel::MasterTrackSnapshot>& snapshot);

    // MIDI dump helpers
    MidiDumpWindow::ClipDumpData buildMidiClipDumpData(int32_t trackIndex, int32_t clipId);
    MidiDumpWindow::ClipDumpData buildMasterMetaDumpData();

    // Build render context
    SequenceEditor::RenderContext buildRenderContext(float uiScale);
};

}  // namespace uapmd::gui
