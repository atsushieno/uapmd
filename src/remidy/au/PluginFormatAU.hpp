#pragma once
#include <iostream>
#include <sstream>

#include "remidy.hpp"
#include "../GenericAudioBuses.hpp"
#include "AUv2Helper.hpp"
#include <AVFoundation/AVFoundation.h>
#include <CoreFoundation/CoreFoundation.h>

namespace remidy {
    class PluginScannerAU : public PluginScanning {
        ScanningStrategyValue scanRequiresLoadLibrary() override { return ScanningStrategyValue::NEVER; }
        ScanningStrategyValue scanRequiresInstantiation() override { return ScanningStrategyValue::NEVER; }
        std::vector<std::unique_ptr<PluginCatalogEntry>> scanAllAvailablePlugins() override;
    };

    class PluginFormatAU::Impl {
        PluginFormatAU* format;
        Logger* logger;
        Extensibility extensibility;
        PluginScannerAU scanning_{};
    public:
        Impl(PluginFormatAU* format, Logger* logger) : format(format), logger(logger), extensibility(*format) {}

        Extensibility* getExtensibility() { return &extensibility; }
        PluginScanning* scanning() { return &scanning_; }

        Logger* getLogger() { return logger; }
    };

    class PluginInstanceAU : public PluginInstance {

        class ParameterSupport : public PluginParameterSupport {
            remidy::PluginInstanceAU *owner;
            AudioUnitParameterID* au_param_id_list{nullptr};
            UInt32 au_param_id_list_size{0};
            std::vector<PluginParameter*> parameter_list{};
            std::map<uint32_t,std::vector<PluginParameter*>> parameter_lists_per_note{};

        public:
            explicit ParameterSupport(PluginInstanceAU* owner);
            ~ParameterSupport() override;

            std::vector<PluginParameter*>& parameters() override;
            std::vector<PluginParameter*>& perNoteControllers(PerNoteControllerContextTypes types, PerNoteControllerContext note) override;

            StatusCode setParameter(uint32_t index, double value, uint64_t timestamp) override;
            StatusCode getParameter(uint32_t index, double *value) override;
            StatusCode setPerNoteController(PerNoteControllerContext context, uint32_t index, double value, uint64_t timestamp) override;
            StatusCode getPerNoteController(PerNoteControllerContext context, uint32_t index, double *value) override;
            std::string valueToString(uint32_t index, double value) override;
        };

        class AUUmpInputDispatcher : UmpInputDispatcher {
            remidy::PluginInstanceAU *owner;
            MIDIEventList* ump_event_list{};
        public:
            explicit AUUmpInputDispatcher(remidy::PluginInstanceAU *owner);
            ~AUUmpInputDispatcher() override;

            void process(uint64_t timestamp, remidy::AudioProcessContext &src) override;
        };

        class AudioBuses : public GenericAudioBuses {
            PluginInstanceAU* owner;

        public:
            explicit AudioBuses(PluginInstanceAU* owner) : owner(owner) {
                inspectBuses();
            }

            StatusCode configure(ConfigurationRequest& configuration);

            void inspectBuses() override;
        };

        class PluginStatesAU : public PluginStateSupport {
            PluginInstanceAU* owner;

        public:
            explicit PluginStatesAU(PluginInstanceAU* owner) : owner(owner) {}

            std::vector<uint8_t> getState(StateContextType stateContextType, bool includeUiState) override;
            void setState(std::vector<uint8_t>& state, StateContextType stateContextType, bool includeUiState) override;
        };

        class PresetsSupport : public PluginPresetsSupport {
            PluginInstanceAU *owner;
            std::vector<PresetInfo> items{};

        public:
            explicit PresetsSupport(PluginInstanceAU* owner);
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
            explicit UISupport(PluginInstanceAU* owner);
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
            PluginInstanceAU* owner;
            void* ns_view{nullptr};            // NSView*
            void* ns_window{nullptr};          // NSWindow* - only for floating windows
            void* ns_bundle{nullptr};          // NSBundle* - for AUv2 cleanup
            void* ns_view_controller{nullptr}; // NSViewController* - for AUv3
            bool created{false};
            bool visible{false};
            bool attached{false};
            bool is_floating{false};
            std::function<bool(uint32_t, uint32_t)> host_resize_handler{};
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
            double sampleRate{44100.0};
        };

        void initializeHostCallbacks();
        static OSStatus hostCallbackGetBeatAndTempo(void* inHostUserData, Float64* outCurrentBeat, Float64* outCurrentTempo);
        static OSStatus hostCallbackGetMusicalTimeLocation(void* inHostUserData, UInt32* outDeltaSampleOffsetToNextBeat, Float32* outTimeSigNumerator, UInt32* outTimeSigDenominator, Float64* outCurrentMeasureDownBeat);
        static OSStatus hostCallbackGetTransportState(void* inHostUserData, Boolean* outIsPlaying, Boolean* outTransportStateChanged, Float64* outCurrentSampleInTimeline, Boolean* outIsCycling, Float64* outCycleStartBeat, Float64* outCycleEndBeat);
        static OSStatus hostCallbackGetTransportState2(void* inHostUserData, Boolean* outIsPlaying, Boolean* outIsRecording, Boolean* outTransportStateChanged, Float64* outCurrentSampleInTimeline, Boolean* outIsCycling, Float64* outCycleStartBeat, Float64* outCycleEndBeat);

        OSStatus audioInputRenderCallback(AudioUnitRenderActionFlags *ioActionFlags, const AudioTimeStamp *inTimeStamp, UInt32 inBusNumber, UInt32 inNumberFrames, AudioBufferList *ioData);
        static OSStatus audioInputRenderCallback(void *inRefCon, AudioUnitRenderActionFlags *ioActionFlags, const AudioTimeStamp *inTimeStamp, UInt32 inBusNumber, UInt32 inNumberFrames, AudioBufferList *ioData) {
            return ((PluginInstanceAU *)inRefCon)->audioInputRenderCallback(ioActionFlags, inTimeStamp, inBusNumber, inNumberFrames, ioData);
        }
        OSStatus midiOutputCallback(const AudioTimeStamp *timeStamp, UInt32 midiOutNum, const struct MIDIPacketList *pktlist);
        static OSStatus midiOutputCallback(void *userData, const AudioTimeStamp *timeStamp, UInt32 midiOutNum, const struct MIDIPacketList *pktlist) {
            return ((remidy::PluginInstanceAU*) userData)->midiOutputCallback(timeStamp, midiOutNum, pktlist);
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

    protected:
        PluginFormatAU *format;
        PluginFormat::PluginInstantiationOptions options;
        Logger* logger_;
        AudioComponent component;
        AudioUnit instance;
        std::string name{};

        PluginInstanceAU(PluginFormatAU* format, PluginFormat::PluginInstantiationOptions options, Logger* logger, PluginCatalogEntry* info, AudioComponent component, AudioUnit instance);
        ~PluginInstanceAU() override;

    public:
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
        void setOfflineMode(bool offlineMode) override;

        virtual AUVersion auVersion() = 0;
        virtual StatusCode sampleRate(double sampleRate) = 0;

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

    class PluginInstanceAUv2 final : public PluginInstanceAU {
    public:
        PluginInstanceAUv2(PluginFormatAU* format, PluginFormat::PluginInstantiationOptions options,
                                Logger* logger, PluginCatalogEntry* info, AudioComponent component, AudioUnit instance
        ) : PluginInstanceAU(format, options, logger, info, component, instance) {
        }

        ~PluginInstanceAUv2() override = default;

        AUVersion auVersion() override { return AUV2; }

        StatusCode sampleRate(double sampleRate) override;
    };

    class PluginInstanceAUv3 final : public PluginInstanceAU {
    public:
        PluginInstanceAUv3(PluginFormatAU* format, PluginFormat::PluginInstantiationOptions options,
                                Logger *logger, PluginCatalogEntry* info, AudioComponent component, AudioUnit instance
        ) : PluginInstanceAU(format, options, logger, info, component, instance) {
        }

        ~PluginInstanceAUv3() override = default;

        AUVersion auVersion() override { return AUV3; }

        StatusCode sampleRate(double sampleRate) override;
    };
}
