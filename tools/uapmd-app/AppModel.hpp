#pragma once

#include <atomic>
#include <format>
#include <filesystem>
#include <thread>
#include <string>
#include <unordered_map>
#include <optional>
#include <set>
#include <mutex>
#include <memory>
#undef None
#undef PropertyNotify
#include <midicci/midicci.hpp>
#include <remidy-tooling/PluginScanTool.hpp>
#include <uapmd/uapmd.hpp>
#include <uapmd-data/uapmd-data.hpp>
#include <uapmd-engine/uapmd-engine.hpp>
#include <uapmd-file/IDocumentProvider.hpp>

namespace uapmd {
    // Forward declarations
    class AppModel;

    class TransportController {
        AppModel* appModel_;
        RealtimeSequencer* sequencer_;
        bool isPlaying_ = false;
        bool isPaused_ = false;
        bool isRecording_ = false;
        float volume_ = 0.8f;

    public:
        explicit TransportController(AppModel* appModel, RealtimeSequencer* sequencer);

        // State getters
        bool isPlaying() const { return isPlaying_; }
        bool isPaused() const { return isPaused_; }
        bool isRecording() const { return isRecording_; }
        float volume() const { return volume_; }

        // State setters
        void setVolume(float volume) { volume_ = volume; }

        // Transport control methods
        void play();
        void stop();
        void pause();
        void resume();
        void record();
    };

    class AppModel : MidiIOManagerFeature {
        std::shared_ptr<MidiIOFeature> createMidiIOFeature(
            std::string apiName, std::string deviceName, std::string manufacturer, std::string version) override;

    public:
        // Plugin instance metadata
        struct PluginInstanceState {
            std::string pluginName;
            std::string pluginFormat;
            std::string pluginId;
            std::string statusMessage;
            bool instantiating = true;
            bool hasError = false;
            int32_t instanceId = -1;
        };

        // Device state containing MIDI device and associated plugin instances
        struct DeviceState {
            std::mutex mutex;
            std::shared_ptr<UapmdFunctionBlock> device;
            std::string label;
            std::string apiName;
            std::string statusMessage;
            bool running = false;
            bool instantiating = false;
            bool hasError = false;
            std::unordered_map<int32_t, PluginInstanceState> pluginInstances;
        };

        // Device entry wrapper for container management
        struct DeviceEntry {
            int id;
            std::shared_ptr<DeviceState> state;
        };

        struct TrackLayoutChange {
            enum class Type {
                Added,
                Removed,
                Cleared
            };

            Type type{Type::Added};
            int32_t trackIndex{-1};
        };

    private:
        RealtimeSequencer sequencer_;
        remidy_tooling::PluginScanTool pluginScanTool_;
        std::unique_ptr<TransportController> transportController_;
        std::atomic<bool> isScanning_{false};
        mutable std::mutex devicesMutex_;
        std::vector<DeviceEntry> devices_;
        int32_t nextDeviceId_ = 1;

        int32_t sample_rate_;
        uint32_t audio_buffer_size_;
        std::unique_ptr<IDocumentProvider> documentProvider_;
        int32_t next_source_node_id_ = 1;  // Used only by addDeviceInputToTrack
        std::set<int32_t> hidden_tracks_;


        // Audio processing callback (called by SequencerEngine)
        void notifyTrackLayoutChanged(const TrackLayoutChange& change);

    public:
        static void instantiate();
        static AppModel& instance();
        static void cleanupInstance();
        AppModel(size_t audioBufferSizeInFrames, size_t umpBufferSizeInBytes, int32_t sampleRate, DeviceIODispatcher* dispatcher);
        ~AppModel() override = default;

        RealtimeSequencer& sequencer() { return sequencer_; }
        remidy_tooling::PluginScanTool& pluginScanTool() { return pluginScanTool_; }
        TransportController& transport() { return *transportController_; }
        IDocumentProvider* documentProvider() {
            if (!documentProvider_)
                documentProvider_ = createDocumentProvider();
            return documentProvider_.get();
        }
        bool isScanning() const { return isScanning_; }

        std::vector<std::function<void(bool success, std::string error)>> scanningCompleted{};

        // Configuration for creating a plugin instance with virtual MIDI device
        struct PluginInstanceConfig {
            std::string apiName = "default";
            std::string deviceName;  // Empty = auto-generate from plugin name
            std::string manufacturer = "UAPMD Project";
            std::string version = "0.1";
            std::filesystem::path stateFile;
        };

        // Result from plugin instance creation
        struct PluginInstanceResult {
            int32_t instanceId = -1;
            std::shared_ptr<UapmdFunctionBlock> device;
            std::string pluginName;
            std::string error;
        };

        // Global callback registry - called whenever ANY instance is created (GUI or script)
        std::vector<std::function<void(const PluginInstanceResult&)>> instanceCreated{};

        // Global callback registry - called whenever ANY instance is removed (GUI or script)
        std::vector<std::function<void(int32_t instanceId)>> instanceRemoved{};

        // Result from device enable/disable operations
        struct DeviceStateResult {
            int32_t instanceId = -1;
            bool success = false;
            bool running = false;
            std::string statusMessage;
            std::string error;
        };

        // Global callback registry - called whenever a device is enabled
        std::vector<std::function<void(const DeviceStateResult&)>> enableDeviceCompleted{};

        // Global callback registry - called whenever a device is disabled
        std::vector<std::function<void(const DeviceStateResult&)>> disableDeviceCompleted{};

        // Track layout change notifications
        std::vector<std::function<void(const TrackLayoutChange&)>> trackLayoutChanged{};

        // Create plugin instance with virtual MIDI device
        // Notifies all registered callbacks when complete
        // Optional completionCallback is called after successful creation, before global callbacks
        void createPluginInstanceAsync(const std::string& format,
                                       const std::string& pluginId,
                                       int32_t trackIndex,
                                       const PluginInstanceConfig& config,
                                       std::function<void(const PluginInstanceResult&)> completionCallback = nullptr);

        // Remove plugin instance and its virtual MIDI device
        // Notifies all registered callbacks when complete
        void removePluginInstance(int32_t instanceId);

        // Enable virtual MIDI device for an instance
        // Notifies all registered callbacks when complete
        void enableUmpDevice(int32_t instanceId, const std::string& deviceName);

        // Disable virtual MIDI device for an instance
        // Notifies all registered callbacks when complete
        void disableUmpDevice(int32_t instanceId);

        // Query device state for display
        std::vector<DeviceEntry> getDevices() const;
        std::optional<std::shared_ptr<DeviceState>> getDeviceForInstance(int32_t instanceId) const;

        // Update device label from GUI
        void updateDeviceLabel(int32_t instanceId, const std::string& label);

        // Result from UI show/hide operations
        struct UIStateResult {
            int32_t instanceId = -1;
            bool success = false;
            bool visible = false;
            bool wasCreated = false;  // True if createUI() was called
            void* uiHandle = nullptr;  // UI window handle from createUI()
            std::string error;
        };

        // Global callback registry - called when a plugin UI is shown
        std::vector<std::function<void(const UIStateResult&)>> uiShown{};

        // Global callback registry - called when a plugin UI is hidden
        std::vector<std::function<void(const UIStateResult&)>> uiHidden{};

        // Global callback registry - called when user requests to show UI (from scripts)
        // MainWindow should handle this by preparing window and calling showPluginUI()
        std::vector<std::function<void(int32_t instanceId)>> uiShowRequested{};

        // Request to show plugin UI (from scripts) - triggers uiShowRequested callbacks
        void requestShowPluginUI(int32_t instanceId);

        // Show plugin UI - creates (if needsCreate) and shows the UI
        // Notifies all registered callbacks when complete
        void showPluginUI(int32_t instanceId, bool needsCreate, bool isFloating, void* parentHandle, std::function<bool(uint32_t, uint32_t)> resizeHandler);

        // Hide plugin UI
        // Notifies all registered callbacks when complete
        void hidePluginUI(int32_t instanceId);

        void performPluginScanning(bool forceRescan = false);

        // Plugin state save/load operations
        // These handle the file I/O and plugin state manipulation, but not UI dialogs
        struct PluginStateResult {
            int32_t instanceId = -1;
            bool success = false;
            std::string error;
            std::string filepath;  // Path that was used for save/load
        };

        // Load plugin state from a file
        // Returns result with success status and any error message
        PluginStateResult loadPluginState(int32_t instanceId, const std::string& filepath);

        // Save plugin state to a file
        // Returns result with success status and any error message
        PluginStateResult savePluginState(int32_t instanceId, const std::string& filepath);

        // Timeline access
        uapmd::TimelineState& timeline() { return sequencer_.engine()->timeline().state(); }
        const uapmd::TimelineState& timeline() const { return const_cast<AppModel*>(this)->sequencer_.engine()->timeline().state(); }

        // Clip management
        struct ClipAddResult {
            int32_t clipId = -1;
            int32_t sourceNodeId = -1;
            bool success = false;
            std::string error;
        };

        ClipAddResult addClipToTrack(
            int32_t trackIndex,
            const uapmd::TimelinePosition& position,
            std::unique_ptr<uapmd::AudioFileReader> reader,
            const std::string& filepath = ""
        );

        ClipAddResult addMidiClipToTrack(
            int32_t trackIndex,
            const uapmd::TimelinePosition& position,
            const std::string& filepath
        );

        // Add MIDI clip with pre-converted UMP data (for multi-track SMF import)
        ClipAddResult addMidiClipToTrack(
            int32_t trackIndex,
            const uapmd::TimelinePosition& position,
            std::vector<uapmd_ump_t> umpEvents,
            std::vector<uint64_t> umpTickTimestamps,
            uint32_t tickResolution,
            double clipTempo,
            std::vector<MidiTempoChange> tempoChanges,
            std::vector<MidiTimeSignatureChange> timeSignatureChanges,
            const std::string& clipName = ""
        );

        bool removeClipFromTrack(int32_t trackIndex, int32_t clipId);

        // Device input routing
        int32_t addDeviceInputToTrack(
            int32_t trackIndex,
            const std::vector<uint32_t>& channelIndices
        );

        // Timeline tracks access
        std::vector<uapmd::TimelineTrack*> getTimelineTracks();
        size_t trackCount() const { return sequencer_.engine()->tracks().size(); }

        // Track management
        int32_t addTrack();
        bool removeTrack(int32_t trackIndex);
        void removeAllTracks();
        bool isTrackHidden(int32_t trackIndex) const { return hidden_tracks_.contains(trackIndex); }

        // Sample rate access
        int32_t sampleRate() const { return sample_rate_; }

        struct ProjectResult {
            bool success{false};
            std::string error;
        };

        ProjectResult saveProject(const std::filesystem::path& file);
        ProjectResult loadProject(const std::filesystem::path& file);

        struct MasterTrackSnapshot {
            struct TempoPoint {
                double timeSeconds{0.0};
                uint64_t tickPosition{0};
                double bpm{0.0};
            };
            struct TimeSignaturePoint {
                double timeSeconds{0.0};
                uint64_t tickPosition{0};
                MidiTimeSignatureChange signature{};
            };

            std::vector<TempoPoint> tempoPoints;
            std::vector<TimeSignaturePoint> timeSignaturePoints;
            double maxTimeSeconds{0.0};
            bool empty() const {
                return tempoPoints.empty() && timeSignaturePoints.empty();
            }
        };

        MasterTrackSnapshot buildMasterTrackSnapshot();

        struct TimelineContentBounds {
            bool hasContent{false};
            double startSeconds{0.0};
            double endSeconds{0.0};
            double durationSeconds{0.0};
        };

        TimelineContentBounds timelineContentBounds() const;

        struct RenderToFileSettings {
            std::filesystem::path outputPath;
            double startSeconds{0.0};
            std::optional<double> endSeconds;
            bool useContentFallback{false};
            bool contentBoundsValid{false};
            double contentStartSeconds{0.0};
            double contentEndSeconds{0.0};
            double tailSeconds{2.0};
            bool enableSilenceStop{false};
            double silenceDurationSeconds{5.0};
            double silenceThresholdDb{-80.0};
        };

        struct RenderToFileStatus {
            bool running{false};
            bool completed{false};
            bool success{false};
            double progress{0.0};
            double renderedSeconds{0.0};
            std::string message;
            std::filesystem::path outputPath;
        };

        bool startRenderToFile(const RenderToFileSettings& settings);
        void cancelRenderToFile();
        RenderToFileStatus getRenderToFileStatus() const;
        void clearCompletedRenderStatus();

    private:
        struct RenderJobState {
            std::atomic<bool> cancel{false};
        };

        void runRenderToFile(RenderToFileSettings settings, std::shared_ptr<RenderJobState> job);

        mutable std::mutex renderStatusMutex_;
        RenderToFileStatus renderStatus_;
        mutable std::mutex renderJobMutex_;
        std::shared_ptr<RenderJobState> activeRenderJob_;
    };
}
