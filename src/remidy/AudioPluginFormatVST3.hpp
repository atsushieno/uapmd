#pragma once

#include "remidy.hpp"
#include "vst3/HostClasses.hpp"

using namespace remidy_vst3;

namespace remidy {
    class AudioPluginInstanceVST3;

    class AudioPluginFormatVST3::Impl {
        AudioPluginFormatVST3* owner;
        Logger* logger;
        Extensibility extensibility;

        StatusCode doLoad(std::filesystem::path &vst3Dir, void** module) const;
        static StatusCode doUnload(std::filesystem::path &vst3Dir, void* module);
        std::function<StatusCode(std::filesystem::path &vst3Dir, void** module)> loadFunc;
        std::function<StatusCode(std::filesystem::path &vst3Dir, void* module)> unloadFunc;

        PluginBundlePool library_pool;
        HostApplication host;
        void scanAllAvailablePluginsFromLibrary(std::filesystem::path vst3Dir, std::vector<PluginClassInfo>& results);
        std::unique_ptr<PluginCatalogEntry> createPluginInformation(PluginClassInfo& info);

    public:
        explicit Impl(AudioPluginFormatVST3* owner) :
            owner(owner),
            logger(Logger::global()),
            extensibility(*owner),
            loadFunc([&](std::filesystem::path &vst3Dir, void** module)->StatusCode { return doLoad(vst3Dir, module); }),
            unloadFunc([&](std::filesystem::path &vst3Dir, void* module)->StatusCode { return doUnload(vst3Dir, module); }),
            library_pool(loadFunc,unloadFunc),
            host(logger) {
        }

        auto format() const { return owner; }
        Logger* getLogger() { return logger; }
        HostApplication* getHost() { return &host; }

        AudioPluginExtensibility<AudioPluginFormat>* getExtensibility();
        std::vector<std::unique_ptr<PluginCatalogEntry>> scanAllAvailablePlugins();
        void forEachPlugin(std::filesystem::path& vst3Dir,
            const std::function<void(void* module, IPluginFactory* factory, PluginClassInfo& info)>& func,
            const std::function<void(void* module)>& cleanup
        );
        void unrefLibrary(PluginCatalogEntry* info);

        void createInstance(PluginCatalogEntry* info, std::function<void(std::unique_ptr<AudioPluginInstance> instance, std::string error)>&& callback);
        StatusCode configure(int32_t sampleRate);
    };

    class AudioPluginInstanceVST3 : public AudioPluginInstance {
        AudioPluginFormatVST3::Impl* owner;
        PluginCatalogEntry* info;
        void* module;
        IPluginFactory* factory;
        std::string pluginName;
        IComponent* component;
        IAudioProcessor* processor;
        IEditController* controller;
        bool isControllerDistinctFromComponent;
        FUnknown* instance;
        IConnectionPoint* connPointComp{nullptr};
        IConnectionPoint* connPointEdit{nullptr};

        int32_t maxAudioFrameCount = 4096; // FIXME: retrieve appropriate config
        v3_process_data processData{};
        v3_process_context process_context{};
        std::vector<v3_audio_bus_buffers> inputAudioBusBuffersList{};
        std::vector<v3_audio_bus_buffers> outputAudioBusBuffersList{};
        HostEventList processDataInputEvents{};
        HostEventList processDataOutputEvents{};
        HostParameterChanges processDataInputParameterChanges{};
        HostParameterChanges processDataOutputParameterChanges{};

        void allocateProcessData();
        void deallocateProcessData();
        std::vector<v3_speaker_arrangement> getVst3SpeakerConfigs(int32_t direction);

        struct BusSearchResult {
            uint32_t numAudioIn{0};
            uint32_t numAudioOut{0};
            uint32_t numEventIn{0};
            uint32_t numEventOut{0};
        };
        BusSearchResult busesInfo{};
        void inspectBuses();
        std::vector<AudioBusDefinition> input_bus_defs{};
        std::vector<AudioBusDefinition> output_bus_defs{};
        std::vector<AudioBusConfiguration*> input_buses{};
        std::vector<AudioBusConfiguration*> output_buses{};

    public:
        explicit AudioPluginInstanceVST3(
            AudioPluginFormatVST3::Impl* owner,
            PluginCatalogEntry* info,
            void* module,
            IPluginFactory* factory,
            IComponent* component,
            IAudioProcessor* processor,
            IEditController* controller,
            bool isControllerDistinctFromComponent,
            FUnknown* instance
            );
        ~AudioPluginInstanceVST3() override;

        AudioPluginUIThreadRequirement requiresUIThreadOn() override {
            // maybe we add some entries for known issues
            return owner->format()->requiresUIThreadOn(info);
        }

        // audio processing core features
        StatusCode configure(ConfigurationRequest& configuration) override;
        StatusCode startProcessing() override;
        StatusCode stopProcessing() override;
        StatusCode process(AudioProcessContext &process) override;

        // port helpers
        bool hasAudioInputs() override { return busesInfo.numAudioIn > 0; }
        bool hasAudioOutputs() override { return busesInfo.numAudioOut > 0; }
        bool hasEventInputs() override { return busesInfo.numEventIn > 0; }
        bool hasEventOutputs() override { return busesInfo.numEventOut > 0; }

        const std::vector<AudioBusConfiguration*> audioInputBuses() const override;
        const std::vector<AudioBusConfiguration*> audioOutputBuses() const override;
    };

}
