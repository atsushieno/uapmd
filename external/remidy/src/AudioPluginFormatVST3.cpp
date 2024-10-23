
#include <iostream>

#include "remidy.hpp"
#include "utils.hpp"

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
        remidy_vst3::HostApplication host;
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
        void unrefLibrary(PluginCatalogEntry *info);

        void createInstance(PluginCatalogEntry *info, std::function<void(InvokeResult)> callback);
        StatusCode configure(int32_t sampleRate);
    };

    StatusCode AudioPluginFormatVST3::Impl::doLoad(std::filesystem::path &vst3Dir, void** module) const {
        *module = loadModuleFromVst3Path(vst3Dir);
        if (*module) {
            auto err = initializeModule(*module);
            if (err != 0) {
                auto s = vst3Dir.string();
                auto cp = s.c_str();
                logger->logWarning("Could not initialize the module from bundle: %s", cp);
                unloadModule(*module);
                *module = nullptr;
            }
        }

        return *module == nullptr ? StatusCode::FAILED_TO_INSTANTIATE : StatusCode::OK;
    };

    StatusCode AudioPluginFormatVST3::Impl::doUnload(std::filesystem::path &vst3Dir, void* module) {
        unloadModule(module);
        return StatusCode::OK;
    }

    AudioPluginExtensibility<AudioPluginFormat> * AudioPluginFormatVST3::Impl::getExtensibility() {
        return &extensibility;
    }

    std::unique_ptr<PluginCatalogEntry> AudioPluginFormatVST3::Impl::createPluginInformation(PluginClassInfo &info) {
        auto ret = std::make_unique<PluginCatalogEntry>();
        static std::string format{"VST3"};
        ret->format(format);
        auto idString = hexBinaryToString((char*) info.tuid, sizeof(v3_tuid));
        ret->bundlePath(info.bundlePath);
        ret->pluginId(idString);
        ret->setMetadataProperty(PluginCatalogEntry::MetadataPropertyID::DisplayName, info.name);
        ret->setMetadataProperty(PluginCatalogEntry::MetadataPropertyID::VendorName, info.vendor);
        ret->setMetadataProperty(PluginCatalogEntry::MetadataPropertyID::ProductUrl, info.url);
        return std::move(ret);
    }

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
        v3_audio_bus_buffers inputAudioBusBuffers{};
        v3_audio_bus_buffers outputAudioBusBuffers{};
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
        BusSearchResult inspectBuses();
        std::vector<AudioBusConfiguration*> input_buses;
        std::vector<AudioBusConfiguration*> output_buses;

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
            return owner->format()->requiresUIThreadOn();
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

    // AudioPluginFormatVST3

    std::vector<std::filesystem::path>& AudioPluginFormatVST3::getDefaultSearchPaths() {
        static std::filesystem::path defaultSearchPathsVST3[] = {
#if _WIN32
            std::string(getenv("LOCALAPPDATA")) + "\\Programs\\Common\\VST3",
            std::string(getenv("PROGRAMFILES")) + "\\Common Files\\VST3",
            std::string(getenv("PROGRAMFILES(x86)")) + "\\Common Files\\VST3"
#elif __APPLE__
            std::string(getenv("HOME")) + "/Library/Audio/Plug-Ins/VST3",
            "/Library/Audio/Plug-Ins/VST3",
            "/Network/Library/Audio/Plug-Ins/VST3"
#else // We assume the rest covers Linux and other Unix-y platforms
            std::string(getenv("HOME")) + "/.vst3",
            "/usr/lib/vst3",
            "/usr/local/lib/vst3"
#endif
        };
        static std::vector<std::filesystem::path> ret = [] {
            std::vector<std::filesystem::path> paths{};
            paths.append_range(defaultSearchPathsVST3);
            return paths;
        }();
        return ret;
    }

    AudioPluginFormatVST3::Extensibility::Extensibility(AudioPluginFormat &format)
        : AudioPluginExtensibility(format) {
    }

    AudioPluginFormatVST3::AudioPluginFormatVST3(std::vector<std::string> &overrideSearchPaths)
        : DesktopAudioPluginFormat() {
        impl = new Impl(this);
    }
    AudioPluginFormatVST3::~AudioPluginFormatVST3() {
        delete impl;
    }

    AudioPluginExtensibility<AudioPluginFormat>* AudioPluginFormatVST3::getExtensibility() {
        return impl->getExtensibility();
    }

    bool AudioPluginFormatVST3::usePluginSearchPaths() { return true;}

    AudioPluginFormat::ScanningStrategyValue AudioPluginFormatVST3::scanRequiresLoadLibrary() { return MAYBE; }

    AudioPluginFormat::ScanningStrategyValue AudioPluginFormatVST3::scanRequiresInstantiation() { return YES; }

    std::vector<std::unique_ptr<PluginCatalogEntry>> AudioPluginFormatVST3::scanAllAvailablePlugins() {
        return impl->scanAllAvailablePlugins();
    }

    std::vector<std::unique_ptr<PluginCatalogEntry>>  AudioPluginFormatVST3::Impl::scanAllAvailablePlugins() {
        std::vector<std::unique_ptr<PluginCatalogEntry>> ret{};
        std::vector<PluginClassInfo> infos;
        for (auto &path : owner->getDefaultSearchPaths()) {
            std::filesystem::path dir{path};
            if (is_directory(dir)) {
                for (auto& entry : std::filesystem::directory_iterator(dir)) {
                    if (!strcasecmp(entry.path().extension().c_str(), ".vst3")) {
                        scanAllAvailablePluginsFromLibrary(entry.path(), infos);
                    }
                }
            }
        }
        for (auto &info : infos)
            ret.emplace_back(createPluginInformation(info));
        return ret;
    }

    void AudioPluginFormatVST3::createInstance(PluginCatalogEntry *info, std::function<void(InvokeResult)> callback) {
        return impl->createInstance(info, callback);
    }

    void AudioPluginFormatVST3::Impl::createInstance(PluginCatalogEntry *pluginInfo, std::function<void(InvokeResult)> callback) {
        std::unique_ptr<AudioPluginInstanceVST3> ret{nullptr};
        v3_tuid tuid{};
        auto decodedBytes = stringToHexBinary(pluginInfo->pluginId());
        memcpy(&tuid, decodedBytes.c_str(), decodedBytes.size());
        std::string name = pluginInfo->getMetadataProperty(PluginCatalogEntry::DisplayName);

        forEachPlugin(pluginInfo->bundlePath(), [&](void* module, IPluginFactory* factory, PluginClassInfo &info) {
            if (memcmp(info.tuid, tuid, sizeof(v3_tuid)) != 0)
                return;
            IPluginFactory3* factory3{nullptr};
            auto result = factory->vtable->unknown.query_interface(factory, v3_plugin_factory_3_iid, (void**) &factory3);
            if (result == V3_OK) {
                result = factory3->vtable->factory_3.set_host_context(factory3, (v3_funknown**) &host);
                // It seems common that a plugin often "implements IPluginFactory3" and then returns kNotImplemented...
                // In that case, it is not callable anyway, so treat it as if IPluginFactory3 were not queryable.
                factory3->vtable->unknown.unref(factory3);
                if (result != V3_OK) {
                    if (((Extensibility*) getExtensibility())->reportNotImplemented())
                        logger->logWarning("Failed to set HostApplication to IPluginFactory3: %s result: %d", name.c_str(), result);
                    if (result != V3_NOT_IMPLEMENTED)
                        return;
                }
            }

            FUnknown* instance{};
            result = factory->vtable->factory.create_instance(factory, tuid, v3_component_iid, (void**) &instance);
            if (result)
                return;

            IComponent *component{};
            result = instance->vtable->unknown.query_interface(instance, v3_component_iid, (void**) &component);
            if (result != V3_OK) {
                logger->logError("Failed to query VST3 component: %s result: %d", name.c_str(), result);
                instance->vtable->unknown.unref(instance);
                return;
            }

            // From https://steinbergmedia.github.io/vst3_dev_portal/pages/Technical+Documentation/API+Documentation/Index.html#initialization :
            // > Hosts should not call other functions before initialize is called, with the sole exception of Steinberg::Vst::IComponent::setIoMode
            // > which must be called before initialize.
            result = component->vtable->component.set_io_mode(instance, V3_IO_ADVANCED);
            if (result != V3_OK && result != V3_NOT_IMPLEMENTED) {
                logger->logError("Failed to set vst3 I/O mode: %s", name.c_str());
                component->vtable->unknown.unref(component);
                instance->vtable->unknown.unref(instance);
                return;
            }
            IAudioProcessor *processor{};
            result = component->vtable->unknown.query_interface(component, v3_audio_processor_iid, (void**) &processor);
            if (result != V3_OK) {
                logger->logError("Could not query vst3 IAudioProcessor interface: %s (status: %d ", name.c_str(), result);
                component->vtable->unknown.unref(component);
                instance->vtable->unknown.unref(instance);
                return;
            }

            // Now initialize the component, and optionally initialize the controller.
            result = component->vtable->base.initialize(component, (v3_funknown**) &host);
            if (result != V3_OK) {
                logger->logError("Failed to initialize vst3: %s (status: %d ", name.c_str(), result);
                component->vtable->unknown.unref(component);
                instance->vtable->unknown.unref(instance);
                return;
            }
            // If we can instantiate controller from the component, just use it.
            bool controllerDistinct = false;
            IEditController* controller{nullptr};
            bool controllerValid = false;

            // > Steinberg::Vst::IComponent::getControllerClassId can also be called before (See VST 3 Workflow Diagrams).
            // ... is it another "sole exception" ?
            v3_tuid controllerClassId{};
            result = component->vtable->component.get_controller_class_id(instance, controllerClassId);
            if (result == V3_OK && memcmp(tuid, controllerClassId, sizeof(v3_tuid)) != 0)
                controllerDistinct = true;
            else
                memcpy(controllerClassId, tuid, sizeof(v3_tuid));
            result = factory->vtable->factory.create_instance(factory, controllerClassId, v3_edit_controller_iid, (void**) &controller);
            if (result == V3_OK) {
                result = controller->vtable->base.initialize(controller, (v3_funknown**) &host);
                if (result == V3_OK)
                    controllerValid = true;
            }

            if (controllerValid) {
                auto handler = host.getComponentHandler();
                result = controller->vtable->controller.set_component_handler(controller, (v3_component_handler**) handler);
                if (result == V3_OK) {
                    ret = std::make_unique<AudioPluginInstanceVST3>(this, pluginInfo, module, factory, component, processor, controller, controllerDistinct, instance);
                    return;
                }
                logger->logError("Failed to set vst3 component handler: %s", name.c_str());
            }
            else
                logger->logError("Failed to find valid controller vst3: %s", name.c_str());
            if (controller)
                controller->vtable->unknown.unref(controller);
            logger->logError("Failed to instantiate vst3: %s", name.c_str());
            component->vtable->base.terminate(component);
            // regardless of the result, we go on...

            component->vtable->unknown.unref(component);
            instance->vtable->unknown.unref(instance);
        }, [&](void* module) {
            // do not unload library here.
        });
        callback(InvokeResult{std::move(ret), std::string{""}});
    }

    void AudioPluginFormatVST3::Impl::unrefLibrary(PluginCatalogEntry* info) {
        library_pool.removeReference(info->bundlePath());
    }

    // Loader helpers

    void AudioPluginFormatVST3::Impl::forEachPlugin(std::filesystem::path& vst3Dir,
        const std::function<void(void* module, IPluginFactory* factory, PluginClassInfo& info)>& func,
        const std::function<void(void* module)>& cleanup
    ) {
        // JUCE seems to do this, not sure if it is required (not sure if this point is correct either).
        auto savedPath = std::filesystem::current_path();
        std::filesystem::current_path(vst3Dir);

        auto module = this->library_pool.loadOrAddReference(vst3Dir);

        if (module) {
            auto factory = getFactoryFromLibrary(module);
            if (!factory) {
                std::filesystem::current_path(savedPath);
                return;
            }

            // FIXME: we need to retrieve classInfo2, classInfo3, ...
            v3_factory_info fInfo{};
            factory->vtable->factory.get_factory_info(factory, &fInfo);
            for (int i = 0, n = factory->vtable->factory.num_classes(factory); i < n; i++) {
                v3_class_info cls{};
                auto result = factory->vtable->factory.get_class_info(factory, i, &cls);
                if (result == 0) {
                    if (!strcmp(cls.category, kVstAudioEffectClass)) {
                        std::string name = std::string{cls.name}.substr(0, strlen(cls.name));
                        std::string vendor = std::string{fInfo.vendor}.substr(0, strlen(fInfo.vendor));
                        std::string url = std::string{fInfo.url}.substr(0, strlen(fInfo.url));
                        PluginClassInfo info(vst3Dir, vendor, url, name, cls.class_id);
                        func(module, factory, info);
                    }
                }
                else
                    logger->logError("failed to retrieve class info at %d, in %s", i, vst3Dir.c_str());
            }
            cleanup(module);
        }
        else
            logger->logError("Could not load the library from bundle: %s", vst3Dir.c_str());

        std::filesystem::current_path(savedPath);
    }

    void AudioPluginFormatVST3::Impl::scanAllAvailablePluginsFromLibrary(std::filesystem::path vst3Dir, std::vector<PluginClassInfo>& results) {
        // fast path scanning using moduleinfo.json
        if (remidy_vst3::hasModuleInfo(vst3Dir)) {
            for (auto& e : remidy_vst3::getModuleInfo(vst3Dir))
                results.emplace_back(e);
            return;
        }
        forEachPlugin(vst3Dir, [&](void* module, IPluginFactory* factory, PluginClassInfo& pluginInfo) {
            results.emplace_back(pluginInfo);
        }, [&](void* module) {
            library_pool.removeReference(vst3Dir);
        });
    }

    // AudioPluginInstanceVST3

    // FIXME: we should make edit controller lazily loaded.
    //  Some plugins take long time to instantiate IEditController, and it does not make sense for
    //  non-UI-based audio processing like our virtual MIDI devices.
    AudioPluginInstanceVST3::AudioPluginInstanceVST3(
        AudioPluginFormatVST3::Impl* owner,
        PluginCatalogEntry* info,
        void* module,
        IPluginFactory* factory,
        IComponent* component,
        IAudioProcessor* processor,
        IEditController* controller,
        bool isControllerDistinctFromComponent,
        FUnknown* instance
    ) : owner(owner), info(info), module(module), factory(factory),
        component(component), processor(processor), controller(controller),
        isControllerDistinctFromComponent(isControllerDistinctFromComponent), instance(instance) {

        pluginName = info->getMetadataProperty(PluginCatalogEntry::MetadataPropertyID::DisplayName);

        // set up IConnectionPoints
        auto result = component->vtable->unknown.query_interface(component, v3_connection_point_iid, (void**) &connPointComp);
        if (result != V3_OK && result != V3_NO_INTERFACE)
            owner->getLogger()->logError("%s: IComponent failed to return query for IConnectionPoint as expected. Result: %d", pluginName.c_str(), result);
        result = controller->vtable->unknown.query_interface(controller, v3_connection_point_iid, (void**) &connPointEdit);
        if (result != V3_OK && result != V3_NO_INTERFACE)
            owner->getLogger()->logError("%s: IEditController failed to return query for IConnectionPoint as expected. Result: %d", pluginName.c_str(), result);

        // From JUCE interconnectComponentAndController():
        // > Some plugins need to be "connected" to intercommunicate between their implemented classes
        if (isControllerDistinctFromComponent && connPointComp && connPointEdit) {
            EventLoop::asyncRunOnMainThread([&] {
                connPointComp->vtable->connection_point.connect(connPointComp, (v3_connection_point**) connPointEdit);
                connPointEdit->vtable->connection_point.connect(connPointEdit, (v3_connection_point**) connPointComp);
            });
        }

        // not sure if we want to error out here, so no result check.
        processor->vtable->processor.set_processing(processor, false);
        component->vtable->component.set_active(component, false);

        busesInfo = inspectBuses();
    }

    AudioPluginInstanceVST3::~AudioPluginInstanceVST3() {

        processor->vtable->processor.set_processing(processor, false);
        component->vtable->component.set_active(component, false);

        deallocateProcessData();

        if (connPointEdit)
            connPointEdit->vtable->unknown.unref(connPointEdit);
        if (connPointComp)
            connPointComp->vtable->unknown.unref(connPointComp);


        std::function releaseRemaining = [&] {
            processor->vtable->unknown.unref(processor);

            component->vtable->base.terminate(component);
            component->vtable->unknown.unref(component);

            instance->vtable->unknown.unref(instance);

            owner->unrefLibrary(info);
        };

        EventLoop::asyncRunOnMainThread([&] {
            // We cannot safely clean up Component without making sure that we cleaned up IEditController,
            // so if we do it, then do everything in the main thread(!)
            // We should do this until we somehow get a "UI is totally separate from logic" safety criteria in VST3 plugins...
            if (isControllerDistinctFromComponent) {
                controller->vtable->base.terminate(controller);
                controller->vtable->unknown.unref(controller);

                releaseRemaining();
            }
            else
                releaseRemaining();
        });
    }

    /*
    std::vector<v3_speaker_arrangement> convertToVst3SpeakerConfigs(std::vector<BusConfiguration>& srcBuses) {
        std::vector<v3_speaker_arrangement> ret{};
        for (auto& src : srcBuses) {
            v3_speaker_arrangement v;
            if (src == BusConfiguration::mono())
                v = V3_SPEAKER_C;
            else if (src == BusConfiguration::stereo()) {
                v = V3_SPEAKER_L | V3_SPEAKER_R;
            }
            else {
                // FIXME: implement more maybe.
                v = 0; // not supported yet
            }
            ret.emplace_back(v);
        }
        return ret;
    }*/

    /*
    std::vector<v3_speaker_arrangement> AudioPluginInstanceVST3::getVst3SpeakerConfigs(int32_t direction) {
        std::vector<v3_speaker_arrangement> ret{};
        auto n = component->vtable->component.get_bus_count(component, V3_AUDIO, direction);
        for (int32_t i = 0; i < n; i++) {
            v3_bus_info info;
            component->vtable->component.get_bus_info(component, V3_AUDIO, direction, i, &info);
            // We can only guess the speaker arrangement by name...
            switch (info.channel_count) {
                case 1: ret.emplace_back(V3_SPEAKER_C); break;
                case 2: ret.emplace_back(V3_SPEAKER_L | V3_SPEAKER_R); break;
                // FIXME: implement more, but we can only depend on bus_name...
                default: ret.emplace_back(0); break;
            }
        }
        return ret;
    }*/

    StatusCode AudioPluginInstanceVST3::configure(ConfigurationRequest& configuration) {
        // setupProcessing.
        v3_process_setup setup{};
        setup.sample_rate = configuration.sampleRate;
        setup.max_block_size = static_cast<int32_t>(configuration.bufferSizeInSamples);
        setup.symbolic_sample_size = configuration.dataType == AudioContentType::Float64 ? V3_SAMPLE_64 : V3_SAMPLE_32;
        setup.process_mode = configuration.offlineMode ? V3_OFFLINE : V3_REALTIME;

        auto result = processor->vtable->processor.setup_processing(processor, &setup);
        if (result != V3_OK) {
            owner->getLogger()->logError("Failed to call vst3 setup_processing() result: %d", result);
            return StatusCode::FAILED_TO_CONFIGURE;
        }

        // set audio bus configuration, if explicitly specified.
        /*
        if (configuration.inputBuses.has_value() || configuration.outputBuses.has_value()) {
            auto inputBuses = configuration.inputBuses.has_value() ?
                convertToVst3SpeakerConfigs(configuration.inputBuses.value()) : getVst3SpeakerConfigs(V3_INPUT);
            auto outputBuses = configuration.outputBuses.has_value() ?
                convertToVst3SpeakerConfigs(configuration.outputBuses.value()) : getVst3SpeakerConfigs(V3_OUTPUT);
            processor->vtable->processor.set_bus_arrangements(processor,
                inputBuses.data(), (int32_t) inputBuses.size(),
                outputBuses.data(), (int32_t) outputBuses.size());
        }*/
        // We can only process simple buses so far. Keep others disabled.
        if (hasAudioInputs())
            component->vtable->component.activate_bus(component, V3_AUDIO, V3_INPUT, 0, true);
        if (hasAudioOutputs())
            component->vtable->component.activate_bus(component, V3_AUDIO, V3_OUTPUT, 0, true);

        // lastly activate the instance.
        component->vtable->component.set_active(component, true);

        // setup process_data here.
        allocateProcessData();

        return StatusCode::OK;
    }

    void AudioPluginInstanceVST3::allocateProcessData() {
        auto ctx = (v3_process_context*) calloc(sizeof(v3_process_context), 1);
        // FIXME: retrieve these properties by some means.
        ctx->sample_rate = 48000;
        processData.ctx = ctx;

        processData.input_events = (v3_event_list**) processDataInputEvents.asInterface();
        processData.output_events = (v3_event_list**) processDataOutputEvents.asInterface();
        processData.input_params = (v3_param_changes**) processDataInputParameterChanges.asInterface();
        processData.output_params = (v3_param_changes**) processDataOutputParameterChanges.asInterface();

        // FIXME: adjust audio buses and channels appropriately.
        int32_t numInputBuses = hasAudioInputs() ? 1 : 0;
        int32_t numOutputBuses = hasAudioOutputs() ? 1 : 0;
        processData.num_input_buses = numInputBuses;
        processData.num_output_buses = numOutputBuses;
        processData.inputs = &inputAudioBusBuffers;
        processData.outputs = &outputAudioBusBuffers;
        int32_t numChannels = 2;
        int32_t symbolicSampleSize = processData.symbolic_sample_size;
        if (symbolicSampleSize == V3_SAMPLE_32) {
            for (int32_t bus = 0; bus < numInputBuses; bus++) {
                processData.inputs[bus].num_channels = numChannels;
                processData.inputs[bus].channel_buffers_32 = (float**) calloc(sizeof(float*), numChannels);
            }
            for (int32_t bus = 0; bus < numOutputBuses; bus++) {
                processData.outputs[bus].num_channels = numChannels;
                processData.outputs[bus].channel_buffers_32 = (float**) calloc(sizeof(float*), numChannels);
            }
        } else {
            for (int32_t bus = 0; bus < numInputBuses; bus++) {
                processData.inputs[bus].num_channels = numChannels;
                processData.inputs[bus].channel_buffers_64 = (double**) calloc(sizeof(double*), numChannels);
            }
            for (int32_t bus = 0; bus < numOutputBuses; bus++) {
                processData.outputs[bus].num_channels = numChannels;
                processData.outputs[bus].channel_buffers_64 = (double**) calloc(sizeof(double*), numChannels);
            }
        }

        processData.process_mode = V3_REALTIME; // FIXME: assign specified value
        processData.symbolic_sample_size = V3_SAMPLE_32; // FIXME: assign specified value
    }

    void AudioPluginInstanceVST3::deallocateProcessData() {
        // FIXME: adjust audio buses and channels
        int32_t numInputBuses = 1;
        int32_t numOutputBuses = 1;
        int32_t numChannels = 2;
        int32_t symbolicSampleSize = processData.symbolic_sample_size;
        if (symbolicSampleSize == V3_SAMPLE_32) {
            for (int32_t bus = 0; bus < numInputBuses; bus++)
                free(processData.inputs[bus].channel_buffers_32);
            for (int32_t bus = 0; bus < numOutputBuses; bus++)
                free(processData.outputs[bus].channel_buffers_32);
        } else {
            for (int32_t bus = 0; bus < numInputBuses; bus++)
                free(processData.inputs[bus].channel_buffers_64);
            for (int32_t bus = 0; bus < numOutputBuses; bus++)
                free(processData.outputs[bus].channel_buffers_64);
        }

        if (processData.ctx)
            free(processData.ctx);
    }

    StatusCode AudioPluginInstanceVST3::startProcessing() {
        auto result = processor->vtable->processor.set_processing(processor, true);
        // Surprisingly?, some VST3 plugins do not implement this function.
        // We do not prevent them just because of this.
        if (result != V3_OK && result != V3_NOT_IMPLEMENTED) {
            owner->getLogger()->logError("Failed to start vst3 processing. Result: %d", result);
            return StatusCode::FAILED_TO_START_PROCESSING;
        }
        return StatusCode::OK;
    }

    StatusCode AudioPluginInstanceVST3::stopProcessing() {
        auto result = processor->vtable->processor.set_processing(processor, false);
        // regarding V3_NOT_IMPLEMENTED, see startProcessing().
        if (result != V3_OK && result != V3_NOT_IMPLEMENTED) {
            owner->getLogger()->logError("Failed to stop vst3 processing. Result: %d", result);
            return StatusCode::FAILED_TO_STOP_PROCESSING;
        }
        return StatusCode::OK;
    }

    void updateProcessDataBuffers(v3_process_data& processData, v3_audio_bus_buffers& dstBus, AudioBusBufferList* srcBuf) {
        int32_t numChannels = srcBuf->channelCount();
        if (processData.symbolic_sample_size == V3_SAMPLE_32) {
            for (int32_t ch = 0; ch < numChannels; ch++)
                dstBus.channel_buffers_32[ch] = srcBuf->getFloatBufferForChannel(ch);
        } else {
            for (int32_t ch = 0; ch < numChannels; ch++)
                dstBus.channel_buffers_64[ch] = srcBuf->getDoubleBufferForChannel(ch);
        }
    }

    StatusCode AudioPluginInstanceVST3::process(AudioProcessContext &process) {
        // update audio buffer pointers
        const int32_t numFrames = process.frameCount();
        const int32_t numInputBus = process.audioInBusCount();
        const int32_t numOutputBus = process.audioOutBusCount();
        for (int32_t bus = 0; bus < numInputBus; bus++)
            updateProcessDataBuffers(processData, processData.inputs[bus], process.audioIn(bus));
        for (int32_t bus = 0; bus < numOutputBus; bus++)
            updateProcessDataBuffers(processData, processData.outputs[bus], process.audioOut(bus));

        const auto& ctx = processData.ctx;

        processData.nframes = numFrames;
        auto result = processor->vtable->processor.process(processor, &processData);
        if (result != V3_OK) {
            owner->getLogger()->logError("Failed to process vst3 audio. Result: %d", result);
            return StatusCode::FAILED_TO_PROCESS;
        }

        // post-processing
        ctx->continuous_time_in_samples += numFrames;
        return StatusCode::OK;
    }

    AudioPluginInstanceVST3::BusSearchResult AudioPluginInstanceVST3::inspectBuses() {
        BusSearchResult ret{};
        ret.numAudioIn = component->vtable->component.get_bus_count(component, V3_AUDIO, V3_INPUT);
        ret.numAudioOut = component->vtable->component.get_bus_count(component, V3_AUDIO, V3_OUTPUT);
        ret.numEventIn = component->vtable->component.get_bus_count(component, V3_EVENT, V3_INPUT);
        ret.numEventOut = component->vtable->component.get_bus_count(component, V3_EVENT, V3_OUTPUT);

        // FIXME: we need to fill input_buses and output_buses here.

        return ret;
    }

    const std::vector<AudioBusConfiguration*> AudioPluginInstanceVST3::audioInputBuses() const { return input_buses; }
    const std::vector<AudioBusConfiguration*> AudioPluginInstanceVST3::audioOutputBuses() const { return output_buses; }

}
