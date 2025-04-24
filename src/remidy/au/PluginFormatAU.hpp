#pragma once
#include <iostream>
#include <sstream>

#include "remidy.hpp"
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

    class AudioPluginInstanceAU : public PluginInstance {

        class ParameterSupport : public PluginParameterSupport {
            remidy::AudioPluginInstanceAU *owner;
            AudioUnitParameterID* au_param_id_list{nullptr};
            UInt32 au_param_id_list_size{0};
            std::vector<PluginParameter*> parameter_list{};
            std::map<uint32_t,std::vector<PluginParameter*>> parameter_lists_per_note{};

        public:
            explicit ParameterSupport(AudioPluginInstanceAU* owner);
            ~ParameterSupport();

            bool accessRequiresMainThread() override { return false; }
            std::vector<PluginParameter*>& parameters() override;
            std::vector<PluginParameter*>& perNoteControllers(PerNoteControllerContextTypes types, PerNoteControllerContext note) override;

            StatusCode setParameter(uint32_t index, double value, uint64_t timestamp) override;
            StatusCode getParameter(uint32_t index, double *value) override;
            StatusCode setPerNoteController(PerNoteControllerContext context, uint32_t index, double value, uint64_t timestamp) override;
            StatusCode getPerNoteController(PerNoteControllerContext context, uint32_t index, double *value) override;
        };

        class AUUmpInputDispatcher : UmpInputDispatcher {
            remidy::AudioPluginInstanceAU *owner;
            MIDIEventList* ump_event_list{};
        public:
            AUUmpInputDispatcher(remidy::AudioPluginInstanceAU *owner);
            ~AUUmpInputDispatcher();

            void process(uint64_t timestamp, remidy::AudioProcessContext &src) override;
        };

        class AUAudioBuses : public AudioBuses {
            AudioPluginInstanceAU* owner;

            struct BusSearchResult {
                uint32_t numAudioIn{0};
                uint32_t numAudioOut{0};
                bool hasMidiIn{false};
                bool hasMidiOut{false};
            };
            BusSearchResult buses{};
            void inspectBuses();
            std::vector<AudioBusDefinition> input_bus_defs{};
            std::vector<AudioBusDefinition> output_bus_defs{};
            std::vector<AudioBusConfiguration*> input_buses{};
            std::vector<AudioBusConfiguration*> output_buses{};

        public:
            explicit AUAudioBuses(AudioPluginInstanceAU* owner) : owner(owner) {
                inspectBuses();
            }

            StatusCode configure(ConfigurationRequest& configuration);

            bool hasEventInputs() override { return buses.hasMidiIn; }
            bool hasEventOutputs() override { return buses.hasMidiOut; }

            const std::vector<AudioBusConfiguration*>& audioInputBuses() const override { return input_buses; }
            const std::vector<AudioBusConfiguration*>& audioOutputBuses() const override { return output_buses; }
        };

        OSStatus audioInputRenderCallback(AudioUnitRenderActionFlags *ioActionFlags, const AudioTimeStamp *inTimeStamp, UInt32 inBusNumber, UInt32 inNumberFrames, AudioBufferList *ioData);
        static OSStatus audioInputRenderCallback(void *inRefCon, AudioUnitRenderActionFlags *ioActionFlags, const AudioTimeStamp *inTimeStamp, UInt32 inBusNumber, UInt32 inNumberFrames, AudioBufferList *ioData) {
            return ((AudioPluginInstanceAU *)inRefCon)->audioInputRenderCallback(ioActionFlags, inTimeStamp, inBusNumber, inNumberFrames, ioData);
        }
        OSStatus midiOutputCallback(const AudioTimeStamp *timeStamp, UInt32 midiOutNum, const struct MIDIPacketList *pktlist);
        static OSStatus midiOutputCallback(void *userData, const AudioTimeStamp *timeStamp, UInt32 midiOutNum, const struct MIDIPacketList *pktlist) {
            return ((remidy::AudioPluginInstanceAU*) userData)->midiOutputCallback(timeStamp, midiOutNum, pktlist);
        }

        std::vector<::AudioBufferList*> auDataIns{};
        std::vector<::AudioBufferList*> auDataOuts{};
        AudioTimeStamp process_timestamp{};
        bool process_replacing{false};
        AudioContentType audio_content_type{AudioContentType::Float32};

        ParameterSupport* _parameters{nullptr};
        AUAudioBuses* audio_buses{};
        AUUmpInputDispatcher ump_input_dispatcher{this};

    protected:
        PluginFormatAU *format;
        Logger* logger_;
        AudioComponent component;
        AudioUnit instance;
        std::string name{};

        AudioPluginInstanceAU(PluginFormatAU* format, Logger* logger, PluginCatalogEntry* info, AudioComponent component, AudioUnit instance);
        ~AudioPluginInstanceAU() override;

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

        virtual AUVersion auVersion() = 0;
        virtual StatusCode sampleRate(double sampleRate) = 0;

        PluginParameterSupport* parameters() override {
            if (!_parameters) _parameters = new ParameterSupport(this);
            return _parameters;
        }

        AudioBuses* audioBuses() override { return audio_buses; }
    };

    class AudioPluginInstanceAUv2 final : public AudioPluginInstanceAU {
    public:
        AudioPluginInstanceAUv2(PluginFormatAU* format, Logger* logger, PluginCatalogEntry* info, AudioComponent component, AudioUnit instance
        ) : AudioPluginInstanceAU(format, logger, info, component, instance) {
        }

        ~AudioPluginInstanceAUv2() override = default;

        AUVersion auVersion() override { return AUV2; }

        StatusCode sampleRate(double sampleRate) override;
    };

    class AudioPluginInstanceAUv3 final : public AudioPluginInstanceAU {
    public:
        AudioPluginInstanceAUv3(PluginFormatAU* format, Logger *logger, PluginCatalogEntry* info, AudioComponent component, AudioUnit instance
        ) : AudioPluginInstanceAU(format, logger, info, component, instance) {
        }

        ~AudioPluginInstanceAUv3() override = default;

        AUVersion auVersion() override { return AUV3; }

        StatusCode sampleRate(double sampleRate) override;
    };
}
