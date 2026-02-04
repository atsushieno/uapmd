#pragma once
#include <iostream>
#include <optional>
#include <sstream>
#include <utility>
#include <unordered_map>

#include "remidy.hpp"
#include "priv/plugin-format-au.hpp"
#include "../GenericAudioBuses.hpp"
#import <AudioToolbox/AudioToolbox.h>
#import <AVFoundation/AVFoundation.h>

struct MIDIEventList;

namespace remidy {
    class PluginScannerAU : public PluginScanning {
        ScanningStrategyValue scanRequiresLoadLibrary() override { return ScanningStrategyValue::NEVER; }
        ScanningStrategyValue scanRequiresInstantiation() override { return ScanningStrategyValue::NEVER; }
        std::vector<std::unique_ptr<PluginCatalogEntry>> scanAllAvailablePlugins() override;
    };

    class PluginFormatAUImpl : public PluginFormatAU {
        Logger* logger;
        PluginFormatAU::Extensibility extensibility;
        PluginScannerAU scanning_{};
    public:
        explicit PluginFormatAUImpl() : logger(Logger::global()), extensibility(*this) {}
        ~PluginFormatAUImpl() override = default;

        PluginExtensibility<PluginFormat>* getExtensibility() override { return &extensibility; }
        PluginScanning* scanning() override { return &scanning_; }

        void createInstance(PluginCatalogEntry* info,
                            PluginInstantiationOptions options,
                            std::function<void(std::unique_ptr<PluginInstance> instance, std::string error)> callback) override;

        Logger* getLogger() { return logger; }
    };

    class PluginInstanceAUv3 : public PluginInstance {

        class MIDIEventConverter {
            // Pre-allocated storage for events to avoid malloc in audio thread
            static constexpr size_t MAX_EVENTS = 1024;
            AURenderEvent eventStorage[MAX_EVENTS];
            size_t eventCount{0};

        public:
            MIDIEventConverter() = default;

            // Convert UMP events from EventSequence to AURenderEvent linked list
            // Returns pointer to first event, or nullptr if no events
            AURenderEvent* convertUMPToRenderEvents(EventSequence& eventIn, AUEventSampleTime eventSampleTime);
        };

    public:
        class ParameterSupport : public PluginParameterSupport {
            remidy::PluginInstanceAUv3 *owner;
            std::vector<PluginParameter*> parameter_list{};
            std::vector<AUParameterAddress> parameter_addresses{};
            AUParameterObserverToken parameterObserverToken{nil};
            void* parameterChangeObserver{nullptr};
            void* observedParameterTree{nullptr};
            AudioUnit v2AudioUnit{nullptr};
            AUEventListenerRef v2PresetListener{nullptr};

        public:
            explicit ParameterSupport(PluginInstanceAUv3* owner);
            ~ParameterSupport() override;

            std::vector<PluginParameter*>& parameters() override;
            std::vector<PluginParameter*>& perNoteControllers(PerNoteControllerContextTypes types, PerNoteControllerContext note) override;

            StatusCode setParameter(uint32_t index, double value, uint64_t timestamp) override;
            StatusCode getParameter(uint32_t index, double *value) override;
            StatusCode setPerNoteController(PerNoteControllerContext context, uint32_t index, double value, uint64_t timestamp) override;
            StatusCode getPerNoteController(PerNoteControllerContext context, uint32_t index, double *value) override;
            std::string valueToString(uint32_t index, double value) override;
            std::string valueToStringPerNote(PerNoteControllerContext context, uint32_t index, double value) override;
            void refreshParameterMetadata(uint32_t index) override;
            void handleParameterSetChange();
            void notifyParameterValue(uint32_t index, double plainValue) { parameterChangeEvent().notify(index, plainValue); }
            void handleParameterTreeStructureChange(void* treeObject);
        private:
            void populateParameterList();
            void rebuildParameterList();
            void clearParameterList();
            void installParameterObserver();
            void uninstallParameterObserver();
            void installParameterChangeObserver();
            void uninstallParameterChangeObserver();
            void broadcastAllParameterValues();
            void installV2PresetListener();
            void uninstallV2PresetListener();
            static void v2PresetEventCallback(void* refCon, void* object, const AudioUnitEvent* event, UInt64 hostTime, Float32 value);
        };

    private:
        class AudioBuses : public GenericAudioBuses {
            PluginInstanceAUv3* owner;

        public:
            explicit AudioBuses(PluginInstanceAUv3* owner) : owner(owner) {
                inspectBuses();
            }

            StatusCode configure(ConfigurationRequest& configuration);

            void inspectBuses() override;
        };

        class PluginStatesAUv3 : public PluginStateSupport {
            PluginInstanceAUv3* owner;

        public:
            explicit PluginStatesAUv3(PluginInstanceAUv3* owner) : owner(owner) {}

            std::vector<uint8_t> getState(StateContextType stateContextType, bool includeUiState) override;
            void setState(std::vector<uint8_t>& state, StateContextType stateContextType, bool includeUiState) override;
        };

        class PresetsSupport : public PluginPresetsSupport {
            PluginInstanceAUv3 *owner;
            std::vector<PresetInfo> items{};

        public:
            explicit PresetsSupport(PluginInstanceAUv3* owner);
            ~PresetsSupport() override = default;

            bool isIndexStable() override { return false; }
            bool isIndexId() override { return false; }
            int32_t getPresetIndexForId(std::string &id) override;
            int32_t getPresetCount() override;
            PresetInfo getPresetInfo(int32_t index) override;
            void loadPreset(int32_t index) override;
        };

        class UISupport : public PluginUISupport {
        public:
            explicit UISupport(PluginInstanceAUv3* owner);
            ~UISupport() override = default;
            bool hasUI() override;
            bool create(bool isFloating, void* parentHandle, std::function<bool(uint32_t, uint32_t)> resizeHandler) override;
            void destroy() override;
            bool show() override;
            void hide() override;
            void setWindowTitle(std::string title) override;
            bool canResize() override;
            bool getSize(uint32_t &width, uint32_t &height) override;
            bool setSize(uint32_t width, uint32_t height) override;
            bool suggestSize(uint32_t &width, uint32_t &height) override;
            bool setScale(double scale) override;
        private:
            PluginInstanceAUv3* owner;
            void* ns_view_controller{nullptr}; // AUViewController*
            void* ns_view{nullptr};             // NSView*
            void* ns_window{nullptr};           // NSWindow* - only for floating windows
            void* view_resize_observer{nullptr}; // NSNotification observer
            bool created{false};
            bool visible{false};
            bool attached{false};
            bool is_floating{false};
            std::function<bool(uint32_t, uint32_t)> host_resize_handler{};
            bool ignore_view_notifications{false};
            bool last_view_size_valid{false};
            double last_view_width{0.0};
            double last_view_height{0.0};

            void startViewResizeObservation(void* viewHandle);
            void stopViewResizeObservation();
            void handleViewSizeChange();
        };

        struct HostTransportInfo {
            double currentBeat{0.0};
            double currentTempo{120.0};
            double currentSample{0.0};
            double cycleStart{0.0};
            double cycleEnd{0.0};
            uint32_t timeSigNumerator{4};
            uint32_t timeSigDenominator{4};
            bool isPlaying{false};
            bool isRecording{false};
            bool isCycling{false};
            bool transportStateChanged{false};
            double sampleRate{48000.0};
        };

        void initializeHostCallbacks();

        ParameterSupport* _parameters{nullptr};
        PluginStatesAUv3* _states{};
        PresetsSupport* _presets{};
        PluginUISupport* _ui{};
        AudioBuses* audio_buses{};
        HostTransportInfo host_transport_info{};

        // MIDI event converter - converts UMP to AURenderEvent
        MIDIEventConverter* midiConverter{nullptr};
        AudioUnit bridgedAudioUnit{nullptr};

        // Temporary buffer for MIDI output events during processing (as uint32_t words)
        std::vector<uint32_t> midi_output_buffer{};
        size_t midi_output_count{0};
        MIDIEventList* midi_event_list{nullptr};
        size_t midi_event_list_capacity{0};

    protected:
        PluginFormatAUImpl *format;
        PluginFormat::PluginInstantiationOptions options;
        Logger* logger_;
        AVAudioUnit* avAudioUnit{nil}; // Wrapper returned by AVFoundation
        AUAudioUnit* audioUnit{nil};  // The AUAudioUnit instance (Objective-C object)
        std::string name{};

    public:
        PluginInstanceAUv3(PluginFormatAUImpl* format,
                           PluginFormat::PluginInstantiationOptions options,
                           Logger* logger,
                           PluginCatalogEntry* info,
                           AVAudioUnit* avAudioUnit,
                           AUAudioUnit* audioUnit);
        ~PluginInstanceAUv3() override;
        Logger* logger() { return logger_; }

        PluginUIThreadRequirement requiresUIThreadOn() override {
            return format->requiresUIThreadOn(info());
        }

        // audio processing core functions.
        StatusCode configure(ConfigurationRequest& configuration) override;
        StatusCode process(AudioProcessContext &process) override;
        StatusCode startProcessing() override;
        StatusCode stopProcessing() override;

        PluginParameterSupport* parameters() override {
            if (!_parameters) _parameters = new ParameterSupport(this);
            return _parameters;
        }

        PluginStatesAUv3* states() override {
            if (!_states) _states = new PluginStatesAUv3(this);
            return _states;
        }

        PresetsSupport* presets() override {
            if (!_presets) _presets = new PresetsSupport(this);
            return _presets;
        }

        PluginUISupport* ui() override {
            if (!_ui) _ui = new UISupport(this);
            return _ui;
        }

        AudioBuses* audioBuses() override { return audio_buses; }
    };

    // AUv2

    class PluginInstanceAUv2 : public PluginInstance {

        class ParameterSupport : public PluginParameterSupport {
            remidy::PluginInstanceAUv2 *owner;
            AudioUnitParameterID* au_param_id_list{nullptr};
            UInt32 au_param_id_list_size{0};
            std::vector<PluginParameter*> parameter_list{};
            std::vector<PluginParameter*> scoped_parameter_list{};
            AUEventListenerRef parameter_listener{nullptr};
            std::unordered_map<AudioUnitParameterID, uint32_t> parameter_id_to_index{};

        public:
            explicit ParameterSupport(PluginInstanceAUv2* owner);
            ~ParameterSupport() override;

            std::vector<PluginParameter*>& parameters() override;
            std::vector<PluginParameter*>& perNoteControllers(PerNoteControllerContextTypes types, PerNoteControllerContext note) override;

            StatusCode setParameter(uint32_t index, double value, uint64_t timestamp) override;
            StatusCode getParameter(uint32_t index, double *value) override;
            StatusCode setPerNoteController(PerNoteControllerContext context, uint32_t index, double value, uint64_t timestamp) override;
            StatusCode getPerNoteController(PerNoteControllerContext context, uint32_t index, double *value) override;
            std::string valueToString(uint32_t index, double value) override;
            std::string valueToStringPerNote(PerNoteControllerContext context, uint32_t index, double value) override;
            void refreshParameterMetadata(uint32_t index) override;
            void refreshAllParameterMetadata() override;
            void notifyParameterValue(uint32_t index, double plainValue) { parameterChangeEvent().notify(index, plainValue); }
        private:
            void rebuildParameterList(bool notifyListeners);
            void clearParameterList();
            void broadcastAllParameterValues();
            void installParameterListener();
            void uninstallParameterListener();
            std::vector<PluginParameter*> buildScopedParameterList(AudioUnitScope scope, UInt32 element);
            std::optional<std::pair<AudioUnitScope, UInt32>> scopeFromContext(PerNoteControllerContextTypes types, PerNoteControllerContext context) const;
            std::optional<PerNoteControllerContextTypes> contextTypeFromScope(AudioUnitScope scope) const;
            static void parameterEventCallback(void* refCon, void* object, const AudioUnitEvent* event, UInt64 hostTime, Float32 value);
            void handleParameterEvent(const AudioUnitEvent* event, Float32 value);
            std::optional<uint32_t> indexForParameterId(AudioUnitParameterID id) const;
        };

        class AUUmpInputDispatcher : UmpInputDispatcher {
            remidy::PluginInstanceAUv2 *owner;
            MIDIEventList* ump_event_list{};
        public:
            explicit AUUmpInputDispatcher(remidy::PluginInstanceAUv2 *owner);
            ~AUUmpInputDispatcher() override;

            void process(remidy::AudioProcessContext &src) override;
        };

        class AudioBuses : public GenericAudioBuses {
            PluginInstanceAUv2* owner;

        public:
            explicit AudioBuses(PluginInstanceAUv2* owner) : owner(owner) {
                inspectBuses();
            }

            StatusCode configure(ConfigurationRequest& configuration);

            void inspectBuses() override;
        };

        class PluginStatesAU : public PluginStateSupport {
            PluginInstanceAUv2* owner;

        public:
            explicit PluginStatesAU(PluginInstanceAUv2* owner) : owner(owner) {}

            std::vector<uint8_t> getState(StateContextType stateContextType, bool includeUiState) override;
            void setState(std::vector<uint8_t>& state, StateContextType stateContextType, bool includeUiState) override;
        };

        class PresetsSupport : public PluginPresetsSupport {
            PluginInstanceAUv2 *owner;
            std::vector<PresetInfo> items{};

        public:
            explicit PresetsSupport(PluginInstanceAUv2* owner);
            ~PresetsSupport() override = default;

            bool isIndexStable() override { return false; }
            bool isIndexId() override { return false; }
            int32_t getPresetIndexForId(std::string &id) override;
            int32_t getPresetCount() override;
            PresetInfo getPresetInfo(int32_t index) override;
            void loadPreset(int32_t index) override;
        };

        class UISupport : public PluginUISupport {
        public:
            explicit UISupport(PluginInstanceAUv2* owner);
            ~UISupport() override = default;
            bool hasUI() override;
            bool create(bool isFloating, void* parentHandle, std::function<bool(uint32_t, uint32_t)> resizeHandler) override;
            void destroy() override;
            bool show() override;
            void hide() override;
            void setWindowTitle(std::string title) override;
            bool canResize() override;
            bool getSize(uint32_t &width, uint32_t &height) override;
            bool setSize(uint32_t width, uint32_t height) override;
            bool suggestSize(uint32_t &width, uint32_t &height) override;
            bool setScale(double scale) override;
        private:
            PluginInstanceAUv2* owner;
            void* ns_view{nullptr};            // NSView*
            void* ns_window{nullptr};          // NSWindow* - only for floating windows
            void* ns_bundle{nullptr};          // NSBundle* - for AUv2 cleanup
            void* ns_view_controller{nullptr}; // NSViewController* - for AUv3
            void* view_resize_observer{nullptr}; // NSNotification observer for view frame changes
            bool created{false};
            bool visible{false};
            bool attached{false};
            bool is_floating{false};
            std::function<bool(uint32_t, uint32_t)> host_resize_handler{};
            bool ignore_view_notifications{false};
            bool last_view_size_valid{false};
            double last_view_width{0.0};
            double last_view_height{0.0};

            void startViewResizeObservation(void* viewHandle);
            void stopViewResizeObservation();
            void handleViewSizeChange();
        };

        struct HostTransportInfo {
            double currentBeat{0.0};
            double currentTempo{120.0};
            double currentSample{0.0};
            double cycleStart{0.0};
            double cycleEnd{0.0};
            uint32_t timeSigNumerator{4};
            uint32_t timeSigDenominator{4};
            bool isPlaying{false};
            bool isRecording{false};
            bool transportStateChanged{false};
            double sampleRate{48000.0};
        };

        void initializeHostCallbacks();
        static OSStatus hostCallbackGetBeatAndTempo(void* inHostUserData, Float64* outCurrentBeat, Float64* outCurrentTempo);
        static OSStatus hostCallbackGetMusicalTimeLocation(void* inHostUserData, UInt32* outDeltaSampleOffsetToNextBeat, Float32* outTimeSigNumerator, UInt32* outTimeSigDenominator, Float64* outCurrentMeasureDownBeat);
        static OSStatus hostCallbackGetTransportState(void* inHostUserData, Boolean* outIsPlaying, Boolean* outTransportStateChanged, Float64* outCurrentSampleInTimeline, Boolean* outIsCycling, Float64* outCycleStartBeat, Float64* outCycleEndBeat);
        static OSStatus hostCallbackGetTransportState2(void* inHostUserData, Boolean* outIsPlaying, Boolean* outIsRecording, Boolean* outTransportStateChanged, Float64* outCurrentSampleInTimeline, Boolean* outIsCycling, Float64* outCycleStartBeat, Float64* outCycleEndBeat);

        OSStatus audioInputRenderCallback(AudioUnitRenderActionFlags *ioActionFlags, const AudioTimeStamp *inTimeStamp, UInt32 inBusNumber, UInt32 inNumberFrames, AudioBufferList *ioData);
        static OSStatus audioInputRenderCallback(void *inRefCon, AudioUnitRenderActionFlags *ioActionFlags, const AudioTimeStamp *inTimeStamp, UInt32 inBusNumber, UInt32 inNumberFrames, AudioBufferList *ioData) {
            return ((PluginInstanceAUv2 *)inRefCon)->audioInputRenderCallback(ioActionFlags, inTimeStamp, inBusNumber, inNumberFrames, ioData);
        }
        OSStatus midiOutputCallback(const AudioTimeStamp *timeStamp, UInt32 midiOutNum, const struct MIDIPacketList *pktlist);
        static OSStatus midiOutputCallback(void *userData, const AudioTimeStamp *timeStamp, UInt32 midiOutNum, const struct MIDIPacketList *pktlist) {
            return ((remidy::PluginInstanceAUv2*) userData)->midiOutputCallback(timeStamp, midiOutNum, pktlist);
        }

        std::vector<::AudioBufferList*> auDataIns{};
        std::vector<::AudioBufferList*> auDataOuts{};
        AudioTimeStamp process_timestamp{};
        bool process_replacing{false};
        AudioContentType audio_content_type{AudioContentType::Float32};

        ParameterSupport* _parameters{nullptr};
        PluginStatesAU* _states{};
        PresetsSupport* _presets{};
        PluginUISupport* _ui{};
        AudioBuses* audio_buses{};
        AURenderCallbackStruct audio_render_callback{};
        AUUmpInputDispatcher ump_input_dispatcher{this};
        HostTransportInfo host_transport_info{};
        HostCallbackInfo host_callback_info{};

        // Temporary buffer for MIDI output events during processing (as uint32_t words)
        std::vector<uint32_t> midi_output_buffer{};
        size_t midi_output_count{0};

        PluginFormatAU *format;
        PluginFormat::PluginInstantiationOptions options;
        Logger* logger_;
        AudioComponent component;
        AudioUnit instance;
        std::string name{};

    public:
        PluginInstanceAUv2(PluginFormatAU* format, PluginFormat::PluginInstantiationOptions options, Logger* logger, PluginCatalogEntry* info, AudioComponent component, AudioUnit instance);
        ~PluginInstanceAUv2() override;

        enum AUVersion {
            AUV2 = 2,
            AUV3 = 3
        };

        Logger* logger() { return logger_; }

        PluginUIThreadRequirement requiresUIThreadOn() override {
            // maybe we add some entries for known issues
            return format->requiresUIThreadOn(info());
        }

        // audio processing core functions.
        StatusCode configure(ConfigurationRequest& configuration) override;
        StatusCode process(AudioProcessContext &process) override;
        StatusCode startProcessing() override;
        StatusCode stopProcessing() override;

        AUVersion auVersion();
        StatusCode sampleRate(double sampleRate);

        PluginParameterSupport* parameters() override {
            if (!_parameters) _parameters = new ParameterSupport(this);
            return _parameters;
        }

        PluginStatesAU* states() override {
            if (!_states) _states = new PluginStatesAU(this);
            return _states;
        }

        PresetsSupport* presets() override {
            if (!_presets) _presets = new PresetsSupport(this);
            return _presets;
        }

        PluginUISupport* ui() override {
            if (!_ui) _ui = new UISupport(this);
            return _ui;
        }

        AudioBuses* audioBuses() override { return audio_buses; }
    };

}
