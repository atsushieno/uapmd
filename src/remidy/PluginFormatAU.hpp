#pragma once
#include <iostream>
#include <sstream>

#include "remidy.hpp"
#include "auv2/AUv2Helper.hpp"
#include <AVFoundation/AVFoundation.h>
#include <CoreFoundation/CoreFoundation.h>

namespace remidy {
    class PluginScannerAU : public PluginScanner {
        ScanningStrategyValue scanRequiresLoadLibrary() override { return ScanningStrategyValue::NEVER; }
        ScanningStrategyValue scanRequiresInstantiation() override { return ScanningStrategyValue::NEVER; }
        std::vector<std::unique_ptr<PluginCatalogEntry>> scanAllAvailablePlugins() override;
    };

    class PluginFormatAU::Impl {
        PluginFormatAU* format;
        Logger* logger;
        Extensibility extensibility;
        PluginScannerAU au_scanner{};
    public:
        Impl(PluginFormatAU* format, Logger* logger) : format(format), logger(logger), extensibility(*format) {}

        Extensibility* getExtensibility() { return &extensibility; }
        PluginScanner* scanner() { return &au_scanner; }

        Logger* getLogger() { return logger; }
    };

    class AudioPluginInstanceAU : public PluginInstance {

        class ParameterSupport : public PluginParameterSupport {
            remidy::AudioPluginInstanceAU *owner;
            AudioUnitParameterID* parameter_id_list{};
            UInt32 parameter_list_size;
            std::vector<PluginParameter*> parameter_list{};

        public:
            ParameterSupport(AudioPluginInstanceAU* owner);
            std::vector<PluginParameter*> parameters() override;
            StatusCode setParameter(uint32_t index, double value, uint64_t timestamp) override;
            StatusCode getParameter(uint32_t index, double *value) override;
        };

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

        ParameterSupport* _parameters;

    protected:
        PluginFormatAU *format;
        AudioComponent component;
        AudioUnit instance;
        std::string name{};

        AudioPluginInstanceAU(PluginFormatAU* format, PluginCatalogEntry* info, AudioComponent component, AudioUnit instance);
        ~AudioPluginInstanceAU() override;

    public:
        enum AUVersion {
            AUV2 = 2,
            AUV3 = 3
        };

        PluginUIThreadRequirement requiresUIThreadOn() override {
            // maybe we add some entries for known issues
            return format->requiresUIThreadOn(info());
        }

        // audio processing core functions.
        StatusCode configure(ConfigurationRequest& configuration) override;
        StatusCode process(AudioProcessContext &process) override;
        StatusCode startProcessing() override;
        StatusCode stopProcessing() override;

        // port helpers
        bool hasEventInputs() override { return buses.hasMidiIn; }
        bool hasEventOutputs() override { return buses.hasMidiOut; }

        const std::vector<AudioBusConfiguration*> audioInputBuses() const override;
        const std::vector<AudioBusConfiguration*> audioOutputBuses() const override;

        virtual AUVersion auVersion() = 0;
        virtual StatusCode sampleRate(double sampleRate) = 0;

        PluginParameterSupport* parameters() override { return _parameters; }
    };

    class AudioPluginInstanceAUv2 final : public AudioPluginInstanceAU {
    public:
        AudioPluginInstanceAUv2(PluginFormatAU* format, PluginCatalogEntry* info, AudioComponent component, AudioUnit instance
        ) : AudioPluginInstanceAU(format, info, component, instance) {
        }

        ~AudioPluginInstanceAUv2() override = default;

        AUVersion auVersion() override { return AUV2; }

        StatusCode sampleRate(double sampleRate) override;
    };

    class AudioPluginInstanceAUv3 final : public AudioPluginInstanceAU {
    public:
        AudioPluginInstanceAUv3(PluginFormatAU* format, PluginCatalogEntry* info, AudioComponent component, AudioUnit instance
        ) : AudioPluginInstanceAU(format, info, component, instance) {
        }

        ~AudioPluginInstanceAUv3() override = default;

        AUVersion auVersion() override { return AUV3; }

        StatusCode sampleRate(double sampleRate) override;
    };
}
