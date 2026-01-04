#pragma once
#include <iostream>
#include <optional>
#include <sstream>
#include <utility>
#include <unordered_map>

#include "remidy.hpp"
#include "priv/plugin-format-auv3.hpp"
#include "../GenericAudioBuses.hpp"
#import <AudioToolbox/AudioToolbox.h>
#import <AVFoundation/AVFoundation.h>

struct MIDIEventList;

namespace remidy {
    class PluginFormatAUv3Impl;

    class PluginScannerAUv3 : public PluginScanning {
        ScanningStrategyValue scanRequiresLoadLibrary() override { return ScanningStrategyValue::NEVER; }
        ScanningStrategyValue scanRequiresInstantiation() override { return ScanningStrategyValue::NEVER; }
        std::vector<std::unique_ptr<PluginCatalogEntry>> scanAllAvailablePlugins() override;
    };

    class PluginFormatAUv3Impl : public PluginFormatAUv3 {
        Logger* logger;
        PluginFormatAUv3::Extensibility extensibility;
        PluginScannerAUv3 scanning_{};
    public:
        explicit PluginFormatAUv3Impl() : logger(Logger::global()), extensibility(*this) {}
        ~PluginFormatAUv3Impl() override = default;

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

        class ParameterSupport : public PluginParameterSupport {
            remidy::PluginInstanceAUv3 *owner;
            std::vector<PluginParameter*> parameter_list{};
            std::vector<AUParameterAddress> parameter_addresses{};
            AUParameterObserverToken parameterObserverToken{nil};

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
            void notifyParameterValue(uint32_t index, double plainValue) { notifyParameterChangeListeners(index, plainValue); }
        private:
            void installParameterObserver();
            void uninstallParameterObserver();
        };

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

        // Temporary buffer for MIDI output events during processing (as uint32_t words)
        std::vector<uint32_t> midi_output_buffer{};
        size_t midi_output_count{0};
        MIDIEventList* midi_event_list{nullptr};
        size_t midi_event_list_capacity{0};

    protected:
        PluginFormatAUv3Impl *format;
        PluginFormat::PluginInstantiationOptions options;
        Logger* logger_;
        AUAudioUnit* audioUnit{nil};  // The AUAudioUnit instance (Objective-C object)
        std::string name{};

    public:
        PluginInstanceAUv3(PluginFormatAUv3Impl* format,
                          PluginFormat::PluginInstantiationOptions options,
                          Logger* logger,
                          PluginCatalogEntry* info,
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
}
