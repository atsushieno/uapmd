#pragma once

#include <vector>
#include <string>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include <optional>
#include <array>
#include <filesystem>
#include <imgui.h>
#include <uapmd/uapmd.hpp>
#include "PluginList.hpp"
#include "PluginSelector.hpp"
#include "TrackList.hpp"
#include "AudioDeviceSettings.hpp"
#include "ScriptEditor.hpp"
#include "SpectrumAnalyzer.hpp"
#include "InstanceDetails.hpp"
#include "SequenceEditor.hpp"
#include "MidiDumpWindow.hpp"
#include <remidy-gui/remidy-gui.hpp>
#include <PluginUIHelpers.hpp>
#include "../AppModel.hpp"

// Forward declarations for different window types
struct SDL_Window;
struct GLFWwindow;

namespace uapmd::gui {

struct GuiDefaults {
    std::string pluginName;
    std::string formatName;
    std::string apiName;
    std::string deviceName;
};

class MainWindow {
        bool isOpen_ = true;

        // Device settings
        bool showDeviceSettingsWindow_ = false;
        AudioDeviceSettings audioDeviceSettings_;

        bool showAudioGraphWindow_ = false;

        // Spectrum analyzers
        SpectrumAnalyzer inputSpectrumAnalyzer_;
        SpectrumAnalyzer outputSpectrumAnalyzer_;

        TrackList trackList_;

        // Plugin selection
        bool showPluginSelectorWindow_ = false;
        PluginSelector pluginSelector_;

        // Script editor
        ScriptEditor scriptEditor_;

        // Virtual MIDI device state tracking
        // Device state structures moved to AppModel - use AppModel::DeviceState, etc.

        std::unordered_map<int32_t, std::unique_ptr<remidy::gui::ContainerWindow>> pluginWindows_;
        std::unordered_map<int32_t, bool> pluginWindowEmbedded_;
        std::unordered_map<int32_t, bool> pluginWindowVisible_;  // Track which plugin UIs are currently visible
        std::unordered_map<int32_t, remidy::gui::Bounds> pluginWindowBounds_;
        std::vector<int32_t> pluginWindowsPendingClose_;
        std::unordered_set<int32_t> pluginWindowResizeIgnore_;

        ImGuiStyle baseStyle_{};
        float uiScale_ = 1.0f;
        bool uiScaleDirty_ = false;
        ImVec2 baseWindowSize_ = ImVec2(800.0f, 800.0f);
        ImVec2 lastWindowSize_ = ImVec2(0.0f, 0.0f);
        bool windowSizeRequestPending_ = false;
        bool waitingForWindowResize_ = false;
        ImVec2 requestedWindowSize_ = ImVec2(0.0f, 0.0f);
        std::vector<float> baseFontScales_;
        bool fontScalesCaptured_ = false;
        struct ChildWindowSizeState {
            ImVec2 baseSize{0.0f, 0.0f};
            ImVec2 lastSize{0.0f, 0.0f};
            bool waitingForResize = false;
        };
        std::unordered_map<std::string, ChildWindowSizeState> childWindowSizes_;

        // UMP device name buffers for each instance
        std::unordered_map<int32_t, std::array<char, 128>> umpDeviceNameBuffers_;

        InstanceDetails instanceDetails_;
        SequenceEditor sequenceEditor_;
        MidiDumpWindow midiDumpWindow_;
        std::shared_ptr<uapmd::AppModel::MasterTrackSnapshot> masterTrackSnapshot_;
        std::string masterTrackSignature_;
        struct TempoSegment {
            double startTime{0.0};
            double endTime{0.0};
            double bpm{120.0};
            double accumulatedBeats{0.0};
        };
        std::vector<TempoSegment> tempoSegments_;
        std::string timelineUnitsLabel_ = "seconds";

    public:
        explicit MainWindow(GuiDefaults defaults = {});
        void render(void* window);  // Generic window pointer
        void update();
        bool& isOpen() { return isOpen_; }
        bool consumePendingWindowResize(ImVec2& size);

    private:
        // No native embedding; we use dedicated windows for plugin UIs
        // Device settings
        void updateAudioDeviceSettingsData();
        void refreshDeviceList();
        void applyDeviceSettings();
        void handleAudioDeviceChange();

        // Track UI
        void renderAudioGraphEditorWindow();
        void renderTrackList(const SequenceEditor::RenderContext& context);
        void renderMasterTrackRow(const SequenceEditor::RenderContext& context);
        void renderTrackRow(int32_t trackIndex, const SequenceEditor::RenderContext& context);
        void refreshInstances();
        void refreshAllSequenceEditorTracks();
        void deleteTrack(int32_t trackIndex);
        void handleTrackLayoutChange(const uapmd::AppModel::TrackLayoutChange& change);
        bool handlePluginResizeRequest(int32_t instanceId, uint32_t width, uint32_t height);
        void onPluginWindowResized(int32_t instanceId);
        void onPluginWindowClosed(int32_t instanceId);
        bool fetchPluginUISize(int32_t instanceId, uint32_t &width, uint32_t &height);

        // State save/load
        void savePluginState(int32_t instanceId);
        void loadPluginState(int32_t instanceId);

        // Plugin selection
        void refreshPluginList();

        // Virtual MIDI device management
        void createPluginInstance(const std::string& format, const std::string& pluginId, int32_t trackIndex);

        // TrackList helper methods
        std::optional<TrackInstance> buildTrackInstanceInfo(int32_t instanceId);
        void handleShowUI(int32_t instanceId);
        void handleHideUI(int32_t instanceId);
        void handleEnableDevice(int32_t instanceId, const std::string& deviceName);
        void handleDisableDevice(int32_t instanceId);
        void handleRemoveInstance(int32_t instanceId);

        // Sequence Editor helpers
        void refreshSequenceEditorForTrack(int32_t trackIndex);
        void addClipToTrack(int32_t trackIndex, const std::string& filepath);
        void addClipToTrackAtPosition(int32_t trackIndex, const std::string& filepath, double positionSeconds);
        void removeClipFromTrack(int32_t trackIndex, int32_t clipId);
        void clearAllClipsFromTrack(int32_t trackIndex);
        void updateClip(int32_t trackIndex, int32_t clipId, int32_t anchorId, const std::string& origin, const std::string& position);
        void updateClipName(int32_t trackIndex, int32_t clipId, const std::string& name);
        void changeClipFile(int32_t trackIndex, int32_t clipId);
        void moveClipAbsolute(int32_t trackIndex, int32_t clipId, double seconds);
        void showMidiClipDump(int32_t trackIndex, int32_t clipId);
        void showMasterMetaDump();
        MidiDumpWindow::ClipDumpData buildMidiClipDumpData(int32_t trackIndex, int32_t clipId);
        MidiDumpWindow::ClipDumpData buildMasterMetaDumpData();
        void importSmfTracks();
        void handleSaveProject();
        void handleLoadProject();

        void renderDeviceSettingsWindow();
        void renderPluginSelectorWindow();
        void applyUiScale(float scale);
        void requestWindowResize();
        void captureFontScales();
        void applyFontScaling();
        void setNextChildWindowSize(const std::string& id, ImVec2 defaultBaseSize);
        void updateChildWindowSizeState(const std::string& id);
        void rebuildTempoSegments(const std::shared_ptr<uapmd::AppModel::MasterTrackSnapshot>& snapshot);
        double secondsToTimelineUnits(double seconds) const;
        double timelineUnitsToSeconds(double units) const;
    };
}
