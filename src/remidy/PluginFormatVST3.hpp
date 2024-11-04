#pragma once

#include "remidy.hpp"
#include "vst3/HostClasses.hpp"

using namespace remidy_vst3;

namespace remidy {
    class AudioPluginInstanceVST3;

    class AudioPluginScannerVST3 : public FileBasedPluginScanner {
        void scanAllAvailablePluginsFromLibrary(std::filesystem::path vst3Dir, std::vector<PluginClassInfo>& results);
        std::unique_ptr<PluginCatalogEntry> createPluginInformation(PluginClassInfo& info);

        PluginFormatVST3::Impl* impl{};

    public:
        AudioPluginScannerVST3(PluginFormatVST3::Impl* impl)
            : impl(impl) {
        }
        bool usePluginSearchPaths() override;
        std::vector<std::filesystem::path>& getDefaultSearchPaths() override;
        ScanningStrategyValue scanRequiresLoadLibrary() override;
        ScanningStrategyValue scanRequiresInstantiation() override;
        std::vector<std::unique_ptr<PluginCatalogEntry>> scanAllAvailablePlugins() override;
    };

    class PluginFormatVST3::Impl {
        PluginFormatVST3* owner;
        Logger* logger;
        Extensibility extensibility;
        AudioPluginScannerVST3 vst3_scanner;

        StatusCode doLoad(std::filesystem::path &vst3Dir, void** module) const;
        static StatusCode doUnload(std::filesystem::path &vst3Dir, void* module);
        std::function<StatusCode(std::filesystem::path &vst3Dir, void** module)> loadFunc;
        std::function<StatusCode(std::filesystem::path &vst3Dir, void* module)> unloadFunc;

        PluginBundlePool library_pool;
        HostApplication host;

    public:
        explicit Impl(PluginFormatVST3* owner) :
            owner(owner),
            // FIXME: should be provided by some means
            logger(Logger::global()),
            extensibility(*owner),
            vst3_scanner(this),
            loadFunc([&](std::filesystem::path &vst3Dir, void** module)->StatusCode { return doLoad(vst3Dir, module); }),
            unloadFunc([&](std::filesystem::path &vst3Dir, void* module)->StatusCode { return doUnload(vst3Dir, module); }),
            library_pool(loadFunc,unloadFunc),
            host(logger) {
        }

        auto format() const { return owner; }
        Logger* getLogger() { return logger; }
        HostApplication* getHost() { return &host; }
        PluginBundlePool* libraryPool() { return &library_pool; }

        PluginExtensibility<PluginFormat>* getExtensibility();
        PluginScanner* scanner() { return &vst3_scanner; }
        std::vector<std::unique_ptr<PluginCatalogEntry>> scanAllAvailablePlugins();
        void forEachPlugin(std::filesystem::path& vst3Dir,
            const std::function<void(void* module, IPluginFactory* factory, PluginClassInfo& info)>& func,
            const std::function<void(void* module)>& cleanup
        );
        void unrefLibrary(PluginCatalogEntry* info);

        void createInstance(PluginCatalogEntry* info, std::function<void(std::unique_ptr<PluginInstance> instance, std::string error)>&& callback);
        StatusCode configure(int32_t sampleRate);
    };

    class AudioPluginInstanceVST3 : public PluginInstance {
        PluginFormatVST3::Impl* owner;
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
            PluginFormatVST3::Impl* owner,
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

        PluginUIThreadRequirement requiresUIThreadOn() override {
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