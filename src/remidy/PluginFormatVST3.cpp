
#include <iostream>

#include "remidy.hpp"
#include "utils.hpp"

#include "PluginFormatVST3.hpp"

using namespace remidy_vst3;

namespace remidy {
    // PluginFormatVST3

    PluginFormatVST3::Extensibility::Extensibility(PluginFormat &format)
            : PluginExtensibility(format) {
    }

    PluginFormatVST3::PluginFormatVST3(std::vector<std::string> &overrideSearchPaths) {
        impl = new Impl(this);
    }

    PluginFormatVST3::~PluginFormatVST3() {
        delete impl;
    }

    PluginScanner *PluginFormatVST3::scanner() {
        return impl->scanner();
    }

    PluginExtensibility<PluginFormat> *PluginFormatVST3::getExtensibility() {
        return impl->getExtensibility();
    }

    // Impl

    StatusCode PluginFormatVST3::Impl::doLoad(std::filesystem::path &vst3Dir, void **module) const {
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

    StatusCode PluginFormatVST3::Impl::doUnload(std::filesystem::path &vst3Dir, void *module) {
        unloadModule(module);
        return StatusCode::OK;
    }

    PluginExtensibility<PluginFormat> *PluginFormatVST3::Impl::getExtensibility() {
        return &extensibility;
    }

    void PluginFormatVST3::createInstance(PluginCatalogEntry *info,
                                          std::function<void(std::unique_ptr<PluginInstance> instance,
                                                             std::string error)> callback) {
        return impl->createInstance(info, callback);
    }

    void PluginFormatVST3::Impl::createInstance(PluginCatalogEntry *pluginInfo,
                                                std::function<void(std::unique_ptr<PluginInstance> instance,
                                                                   std::string error)> callback) {
        PluginCatalogEntry *entry = pluginInfo;
        std::unique_ptr<AudioPluginInstanceVST3> ret{nullptr};
        std::string error{};
        v3_tuid tuid{};
        auto decodedBytes = stringToHexBinary(entry->pluginId());
        memcpy(&tuid, decodedBytes.c_str(), decodedBytes.size());
        std::string name = entry->displayName();

        auto bundle = entry->bundlePath();
        forEachPlugin(bundle, [entry, &ret, tuid, name, &error, this](void *module, IPluginFactory *factory,
                                                                      PluginClassInfo &info) {
            if (memcmp(info.tuid, tuid, sizeof(v3_tuid)) != 0)
                return;
            IPluginFactory3 *factory3{nullptr};
            auto result = factory->vtable->unknown.query_interface(factory, v3_plugin_factory_3_iid,
                                                                   (void **) &factory3);
            if (result == V3_OK) {
                result = factory3->vtable->factory_3.set_host_context(factory3, (v3_funknown **) &host);
                // It seems common that a plugin often "implements IPluginFactory3" and then returns kNotImplemented...
                // In that case, it is not callable anyway, so treat it as if IPluginFactory3 were not queryable.
                factory3->vtable->unknown.unref(factory3);
                if (result != V3_OK) {
                    if (((Extensibility *) getExtensibility())->reportNotImplemented())
                        logger->logWarning("Failed to set HostApplication to IPluginFactory3: %s result: %d",
                                           name.c_str(), result);
                    if (result != V3_NOT_IMPLEMENTED)
                        return;
                }
            }

            FUnknown *instance{};
            result = factory->vtable->factory.create_instance(factory, tuid, v3_component_iid, (void **) &instance);
            if (result)
                return;

            IComponent *component{};
            result = instance->vtable->unknown.query_interface(instance, v3_component_iid, (void **) &component);
            if (result != V3_OK) {
                logger->logError("Failed to query VST3 component: %s result: %d", name.c_str(), result);
                instance->vtable->unknown.unref(instance);
                return;
            }

            // From https://steinbergmedia.github.io/vst3_dev_portal/pages/Technical+Documentation/API+Documentation/Index.html#initialization :
            // > Hosts should not call other functions before initialize is called, with the sole exception of Steinberg::Vst::IComponent::setIoMode
            // > which must be called before initialize.
            //
            // Although, none of known plugins use this feature, and the role of this mode looks overlapped with
            // other processing modes. Since it should be considered harmful to set anything before `initialize()`
            // and make it non-updatable, I find this feature a design mistake at Steinberg.
            // So, let's not even try to support this.
#if 0
            result = component->vtable->component.set_io_mode(instance, V3_IO_ADVANCED);
            if (result != V3_OK && result != V3_NOT_IMPLEMENTED) {
                logger->logError("Failed to set vst3 I/O mode: %s", name.c_str());
                component->vtable->unknown.unref(component);
                instance->vtable->unknown.unref(instance);
                return;
            }
#endif

            IAudioProcessor *processor{};
            result = component->vtable->unknown.query_interface(component, v3_audio_processor_iid,
                                                                (void **) &processor);
            if (result != V3_OK) {
                logger->logError("Could not query vst3 IAudioProcessor interface: %s (status: %d ", name.c_str(),
                                 result);
                error = "Could not query vst3 IAudioProcessor interface";
                component->vtable->unknown.unref(component);
                instance->vtable->unknown.unref(instance);
                return;
            }

            // Now initialize the component, and optionally initialize the controller.
            result = component->vtable->base.initialize(component, (v3_funknown **) &host);
            if (result != V3_OK) {
                logger->logError("Failed to initialize vst3: %s (status: %d ", name.c_str(), result);
                error = "Failed to initialize vst3";
                component->vtable->unknown.unref(component);
                instance->vtable->unknown.unref(instance);
                return;
            }
            // If we can instantiate controller from the component, just use it.
            bool controllerDistinct = false;
            IEditController *controller{nullptr};
            bool controllerValid = false;

            // > Steinberg::Vst::IComponent::getControllerClassId can also be called before (See VST 3 Workflow Diagrams).
            // ... is it another "sole exception" ?
            v3_tuid controllerClassId{};
            result = component->vtable->component.get_controller_class_id(instance, controllerClassId);
            if (result == V3_OK && memcmp(tuid, controllerClassId, sizeof(v3_tuid)) != 0)
                controllerDistinct = true;
            else
                memcpy(controllerClassId, tuid, sizeof(v3_tuid));
            result = factory->vtable->factory.create_instance(factory, controllerClassId, v3_edit_controller_iid,
                                                              (void **) &controller);
            if (result == V3_OK) {
                result = controller->vtable->base.initialize(controller, (v3_funknown **) &host);
                if (result == V3_OK)
                    controllerValid = true;
            }

            if (controllerValid) {
                auto handler = host.getComponentHandler();
                result = controller->vtable->controller.set_component_handler(controller,
                                                                              (v3_component_handler **) handler);
                if (result == V3_OK) {
                    ret = std::make_unique<AudioPluginInstanceVST3>(this, entry, module, factory, component, processor,
                                                                    controller, controllerDistinct, instance);
                    return;
                }
                error = "Failed to set vst3 component handler";
            } else
                error = "Failed to find valid controller vst3";
            if (controller)
                controller->vtable->unknown.unref(controller);
            error = "Failed to instantiate vst3";
            component->vtable->base.terminate(component);
            // regardless of the result, we go on...

            component->vtable->unknown.unref(component);
            instance->vtable->unknown.unref(instance);
        }, [&](void *module) {
            // do not unload library here.
        });
        if (ret)
            callback(std::move(ret), error);
        else
            callback(nullptr, std::format("Specified VST3 plugin {} was not found", entry->displayName()));
    }

    void PluginFormatVST3::Impl::unrefLibrary(PluginCatalogEntry *info) {
        library_pool.removeReference(info->bundlePath());
    }

    // PluginScannerVST3

    std::vector<std::filesystem::path> &AudioPluginScannerVST3::getDefaultSearchPaths() {
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
            for (auto &path: defaultSearchPathsVST3)
                paths.emplace_back(path);
            return paths;
        }();
        return ret;
    }

    std::unique_ptr<PluginCatalogEntry> AudioPluginScannerVST3::createPluginInformation(PluginClassInfo &info) {
        auto ret = std::make_unique<PluginCatalogEntry>();
        static std::string format{"VST3"};
        ret->format(format);
        auto idString = hexBinaryToString((char *) info.tuid, sizeof(v3_tuid));
        ret->bundlePath(info.bundlePath);
        ret->pluginId(idString);
        ret->displayName(info.name);
        ret->vendorName(info.vendor);
        ret->productUrl(info.url);
        return ret;
    }

    bool AudioPluginScannerVST3::usePluginSearchPaths() { return true; }

    PluginScanner::ScanningStrategyValue
    AudioPluginScannerVST3::scanRequiresLoadLibrary() { return ScanningStrategyValue::MAYBE; }

    PluginScanner::ScanningStrategyValue
    AudioPluginScannerVST3::scanRequiresInstantiation() { return ScanningStrategyValue::ALWAYS; }

    std::vector<std::unique_ptr<PluginCatalogEntry>> AudioPluginScannerVST3::scanAllAvailablePlugins() {
        std::vector<std::unique_ptr<PluginCatalogEntry>> ret{};
        std::vector<PluginClassInfo> infos;
        for (auto &path: getDefaultSearchPaths()) {
            std::filesystem::path dir{path};
            if (is_directory(dir)) {
                for (auto &entry: std::filesystem::directory_iterator(dir)) {
                    if (!strcasecmp(entry.path().extension().c_str(), ".vst3")) {
                        scanAllAvailablePluginsFromLibrary(entry.path(), infos);
                    }
                }
            }
        }
        for (auto &info: infos)
            ret.emplace_back(createPluginInformation(info));
        return ret;
    }

    void AudioPluginScannerVST3::scanAllAvailablePluginsFromLibrary(std::filesystem::path vst3Dir,
                                                                    std::vector<PluginClassInfo> &results) {
        impl->getLogger()->logInfo("VST3: scanning %s ", vst3Dir.c_str());
        // fast path scanning using moduleinfo.json
        if (remidy_vst3::hasModuleInfo(vst3Dir)) {
            for (auto &e: remidy_vst3::getModuleInfo(vst3Dir))
                results.emplace_back(e);
            return;
        }
        impl->forEachPlugin(vst3Dir, [&](void *module, IPluginFactory *factory, PluginClassInfo &pluginInfo) {
            results.emplace_back(pluginInfo);
        }, [&](void *module) {
            impl->libraryPool()->removeReference(vst3Dir);
        });
    }

    // Loader helpers

    void PluginFormatVST3::Impl::forEachPlugin(std::filesystem::path &vst3Dir,
                                               const std::function<void(void *module, IPluginFactory *factory,
                                                                        PluginClassInfo &info)> &func,
                                               const std::function<void(void *module)> &cleanup
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
                } else
                    logger->logError("failed to retrieve class info at %d, in %s", i, vst3Dir.c_str());
            }
            cleanup(module);
        } else
            logger->logError("Could not load the library from bundle: %s", vst3Dir.c_str());

        std::filesystem::current_path(savedPath);
    }

    // PluginInstanceVST3

    // FIXME: we should make edit controller lazily loaded.
    //  Some plugins take long time to instantiate IEditController, and it does not make sense for
    //  non-UI-based audio processing like our virtual MIDI devices.
    AudioPluginInstanceVST3::AudioPluginInstanceVST3(
            PluginFormatVST3::Impl *owner,
            PluginCatalogEntry *info,
            void *module,
            IPluginFactory *factory,
            IComponent *component,
            IAudioProcessor *processor,
            IEditController *controller,
            bool isControllerDistinctFromComponent,
            FUnknown *instance
    ) : PluginInstance(info), owner(owner), module(module), factory(factory),
        component(component), processor(processor), controller(controller),
        isControllerDistinctFromComponent(isControllerDistinctFromComponent), instance(instance) {

        pluginName = info->displayName();

        // set up IConnectionPoints
        auto result = component->vtable->unknown.query_interface(component, v3_connection_point_iid,
                                                                 (void **) &connPointComp);
        if (result != V3_OK && result != V3_NO_INTERFACE)
            owner->getLogger()->logError(
                    "%s: IComponent failed to return query for IConnectionPoint as expected. Result: %d",
                    pluginName.c_str(), result);
        result = controller->vtable->unknown.query_interface(controller, v3_connection_point_iid,
                                                             (void **) &connPointEdit);
        if (result != V3_OK && result != V3_NO_INTERFACE)
            owner->getLogger()->logError(
                    "%s: IEditController failed to return query for IConnectionPoint as expected. Result: %d",
                    pluginName.c_str(), result);

        // From JUCE interconnectComponentAndController():
        // > Some plugins need to be "connected" to intercommunicate between their implemented classes
        /*
        if (isControllerDistinctFromComponent && connPointComp && connPointComp->vtable && connPointEdit && connPointEdit->vtable) {
            std::atomic<bool> waitHandle{false};
            EventLoop::runTaskOnMainThread([&] {
                result = connPointComp->vtable->connection_point.connect(connPointComp, (v3_connection_point**) connPointEdit);
                if (result != V3_OK) {
                    owner->getLogger()->logWarning(
                            "%s: IConnectionPoint from IComponent failed to interconnect with its IConnectionPoint from IEditController. Result: %d",
                            pluginName.c_str(), result);
                }
                result = connPointEdit->vtable->connection_point.connect(connPointEdit, (v3_connection_point**) connPointComp);
                if (result != V3_OK) {
                    owner->getLogger()->logWarning(
                            "%s: IConnectionPoint from IEditController failed to interconnect with its IConnectionPoint from IComponent. Result: %d",
                            pluginName.c_str(), result);
                }
                waitHandle = true;
                waitHandle.notify_one();
            });
            while (!waitHandle)
                std::this_thread::yield();
        }*/

        // not sure if we want to error out here, so no result check.
        processor->vtable->processor.set_processing(processor, false);
        component->vtable->component.set_active(component, false);

        inspectBuses();
    }

    AudioPluginInstanceVST3::~AudioPluginInstanceVST3() {

        std::function releaseRemaining = [this] {
            processor->vtable->processor.set_processing(processor, false);
            component->vtable->component.set_active(component, false);

            deallocateProcessData();

            if (connPointEdit)
                connPointEdit->vtable->unknown.unref(connPointEdit);
            if (connPointComp)
                connPointComp->vtable->unknown.unref(connPointComp);

            processor->vtable->unknown.unref(processor);

            component->vtable->base.terminate(component);
            component->vtable->unknown.unref(component);

            instance->vtable->unknown.unref(instance);

            owner->unrefLibrary(info());
        };

        std::cerr << "VST3 instance destructor: " << info()->displayName() << std::endl;
        std::atomic<bool> waitHandle{false};
        EventLoop::runTaskOnMainThread([&] {
            if (isControllerDistinctFromComponent) {
                controller->vtable->base.terminate(controller);
                controller->vtable->unknown.unref(controller);
            }
            releaseRemaining();
            waitHandle = true;
            waitHandle.notify_one();
        });
        std::cerr << "  waiting for cleanup: " << info()->displayName() << std::endl;
        while (!waitHandle)
            std::this_thread::yield();
        std::cerr << "  cleanup done: " << info()->displayName() << std::endl;

        for (const auto bus: input_buses)
            delete bus;
        for (const auto bus: output_buses)
            delete bus;

        if (_parameters)
            delete _parameters;
    }

    v3_speaker_arrangement toVstSpeakerArrangement(AudioChannelLayout src) {
        v3_speaker_arrangement ret{0};
        if (src == AudioChannelLayout::mono())
            ret = V3_SPEAKER_C;
        else if (src == AudioChannelLayout::stereo())
            ret = V3_SPEAKER_L | V3_SPEAKER_R;
        // FIXME: implement more
        return ret;
    }

    StatusCode AudioPluginInstanceVST3::configure(ConfigurationRequest &configuration) {
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

        std::vector<v3_speaker_arrangement> inArr{input_buses.size()};
        for (const auto &input_buse: input_buses)
            inArr.emplace_back(toVstSpeakerArrangement(input_buse->channelLayout()));
        std::vector<v3_speaker_arrangement> outArr{output_buses.size()};
        for (const auto &output_buse: output_buses)
            outArr.emplace_back(toVstSpeakerArrangement(output_buse->channelLayout()));

        // set audio bus configuration, if explicitly specified.
        processor->vtable->processor.set_bus_arrangements(processor,
                                                          inArr.data(), static_cast<int32_t>(inArr.size()),
                                                          outArr.data(), static_cast<int32_t>(outArr.size()));
        for (size_t i = 0, n = input_buses.size(); i < n; ++i)
            component->vtable->component.activate_bus(component, V3_AUDIO, V3_INPUT, i, input_buses[i]->enabled());
        for (size_t i = 0, n = output_buses.size(); i < n; ++i)
            component->vtable->component.activate_bus(component, V3_AUDIO, V3_OUTPUT, i, output_buses[i]->enabled());

        // setup process_data here.
        allocateProcessData(setup);

        return StatusCode::OK;
    }

    void AudioPluginInstanceVST3::allocateProcessData(v3_process_setup& setup) {
        // FIXME: retrieve these properties by some means.
        processData.ctx = &process_context;
        process_context.sample_rate = setup.sample_rate;

        processData.input_events = (v3_event_list **) processDataInputEvents.asInterface();
        processData.output_events = (v3_event_list **) processDataOutputEvents.asInterface();
        processData.input_params = (v3_param_changes **) processDataInputParameterChanges.asInterface();
        processData.output_params = (v3_param_changes **) processDataOutputParameterChanges.asInterface();

        // FIXME: adjust audio buses and channels appropriately.
        inputAudioBusBuffersList.resize(input_buses.size());
        outputAudioBusBuffersList.resize(output_buses.size());

        int32_t numInputBuses = input_buses.size();
        int32_t numOutputBuses = output_buses.size();
        processData.num_input_buses = numInputBuses;
        processData.num_output_buses = numOutputBuses;
        processData.inputs = inputAudioBusBuffersList.data();
        processData.outputs = outputAudioBusBuffersList.data();
        int32_t numChannels = 2;
        int32_t symbolicSampleSize = processData.symbolic_sample_size;
        for (int32_t bus = 0; bus < numInputBuses; bus++) {
            inputAudioBusBuffersList[bus].num_channels = numChannels;
            if (symbolicSampleSize == V3_SAMPLE_32)
                inputAudioBusBuffersList[bus].channel_buffers_32 = (float **) calloc(sizeof(float *), numChannels);
            else
                inputAudioBusBuffersList[bus].channel_buffers_64 = (double **) calloc(sizeof(double *), numChannels);
        }
        for (int32_t bus = 0; bus < numOutputBuses; bus++) {
            outputAudioBusBuffersList[bus].num_channels = numChannels;
            if (symbolicSampleSize == V3_SAMPLE_32)
                outputAudioBusBuffersList[bus].channel_buffers_32 = (float **) calloc(sizeof(float *), numChannels);
            else
                outputAudioBusBuffersList[bus].channel_buffers_64 = (double **) calloc(sizeof(double *), numChannels);
        }

        processData.process_mode = V3_REALTIME; // FIXME: assign specified value
        processData.symbolic_sample_size = V3_SAMPLE_32; // FIXME: assign specified value
    }

    // We cannot "free" pointers on processData because they might get updated by the
    // plugin instance (e.g. by "processReplacing").
    // We allocate memory in inputAudioBusBuffersList and outputAudioBusBuffersList.
    void AudioPluginInstanceVST3::deallocateProcessData() {
        // FIXME: adjust audio buses and channels
        int32_t numInputBuses = input_buses.size();
        int32_t numOutputBuses = output_buses.size();
        int32_t symbolicSampleSize = processData.symbolic_sample_size;
        if (symbolicSampleSize == V3_SAMPLE_32) {
            for (int32_t bus = 0; bus < numInputBuses; bus++)
                if (inputAudioBusBuffersList[bus].channel_buffers_32)
                    free(inputAudioBusBuffersList[bus].channel_buffers_32);
        } else {
            for (int32_t bus = 0; bus < numInputBuses; bus++)
                if (inputAudioBusBuffersList[bus].channel_buffers_64)
                    free(inputAudioBusBuffersList[bus].channel_buffers_64);
        }
        if (symbolicSampleSize == V3_SAMPLE_32) {
            for (int32_t bus = 0; bus < numOutputBuses; bus++)
                if (outputAudioBusBuffersList[bus].channel_buffers_32)
                    free(outputAudioBusBuffersList[bus].channel_buffers_32);
        } else {
            for (int32_t bus = 0; bus < numOutputBuses; bus++)
                if (outputAudioBusBuffersList[bus].channel_buffers_64)
                    free(outputAudioBusBuffersList[bus].channel_buffers_64);
        }
    }

    StatusCode AudioPluginInstanceVST3::startProcessing() {
        // activate the instance.
        component->vtable->component.set_active(component, true);

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

        component->vtable->component.set_active(component, false);
        return StatusCode::OK;
    }

    void
    updateProcessDataBuffers(v3_process_data &processData, v3_audio_bus_buffers &dstBus, AudioBusBufferList *srcBuf) {
        int32_t numChannels = dstBus.num_channels;
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
        const int32_t numInputBus = processData.num_input_buses;
        const int32_t numOutputBus = processData.num_output_buses;
        for (int32_t bus = 0; bus < numInputBus; bus++)
            updateProcessDataBuffers(processData, processData.inputs[bus], process.audioIn(bus));
        for (int32_t bus = 0; bus < numOutputBus; bus++)
            updateProcessDataBuffers(processData, processData.outputs[bus], process.audioOut(bus));

        const auto &ctx = processData.ctx;

        processData.nframes = numFrames;

        // handle UMP inputs via UmpInputDispatcher.
        processDataInputEvents.clear();
        // FIXME: pass correct timestamp
        ump_input_dispatcher.process(0, process);

        // invoke plugin process
        auto result = processor->vtable->processor.process(processor, &processData);

        if (result != V3_OK) {
            owner->getLogger()->logError("Failed to process vst3 audio. Result: %d", result);
            return StatusCode::FAILED_TO_PROCESS;
        }

        // post-processing
        ctx->continuous_time_in_samples += numFrames;

        // FiXME: generate UMP outputs here
        processDataOutputEvents.clear();

        return StatusCode::OK;
    }

    AudioChannelLayout fromVst3SpeakerArrangement(v3_speaker_arrangement src) {
        uint32_t channels = 0;
        for (int32_t i = 0; i < 19; i++)
            if (src & (1 << i))
                channels++;
        return AudioChannelLayout{"", channels};
    }

    void AudioPluginInstanceVST3::inspectBuses() {
        BusSearchResult ret{};
        auto numAudioIn = component->vtable->component.get_bus_count(component, V3_AUDIO, V3_INPUT);
        auto numAudioOut = component->vtable->component.get_bus_count(component, V3_AUDIO, V3_OUTPUT);
        ret.numEventIn = component->vtable->component.get_bus_count(component, V3_EVENT, V3_INPUT);
        ret.numEventOut = component->vtable->component.get_bus_count(component, V3_EVENT, V3_OUTPUT);

        input_bus_defs.clear();
        output_bus_defs.clear();
        for (auto bus: input_buses)
            delete bus;
        for (auto bus: output_buses)
            delete bus;
        input_buses.clear();
        output_buses.clear();
        v3_bus_info info;
        for (uint32_t bus = 0; bus < numAudioIn; bus++) {
            component->vtable->component.get_bus_info(component, V3_AUDIO, V3_INPUT, bus, &info);
            auto name = vst3StringToStdString(info.bus_name);
            auto def = AudioBusDefinition{name, info.flags & V3_MAIN ? AudioBusRole::Main : AudioBusRole::Aux};
            input_bus_defs.emplace_back(def);
            auto conf = new AudioBusConfiguration(def);
            v3_speaker_arrangement arr;
            processor->vtable->processor.get_bus_arrangement(processor, V3_INPUT, bus, &arr);
            conf->channelLayout(fromVst3SpeakerArrangement(arr));
            input_buses.emplace_back(conf);
        }
        for (uint32_t bus = 0; bus < numAudioOut; bus++) {
            component->vtable->component.get_bus_info(component, V3_AUDIO, V3_OUTPUT, bus, &info);
            auto name = vst3StringToStdString(info.bus_name);
            auto def = AudioBusDefinition{name, info.flags & V3_MAIN ? AudioBusRole::Main : AudioBusRole::Aux};
            output_bus_defs.emplace_back(def);
            auto conf = new AudioBusConfiguration(def);
            v3_speaker_arrangement arr;
            processor->vtable->processor.get_bus_arrangement(processor, V3_OUTPUT, bus, &arr);
            conf->channelLayout(fromVst3SpeakerArrangement(arr));
            output_buses.emplace_back(conf);
        }

        busesInfo = ret;
    }

    const std::vector<AudioBusConfiguration*>& AudioPluginInstanceVST3::audioInputBuses() const { return input_buses; }

    const std::vector<AudioBusConfiguration*>& AudioPluginInstanceVST3::audioOutputBuses() const { return output_buses; }

    PluginParameterSupport* AudioPluginInstanceVST3::parameters() {
        if (!_parameters)
            _parameters = new ParameterSupport(this);
        return _parameters;
    }
}

// AudioPluginInstanceVST3::ParameterSupport

remidy::AudioPluginInstanceVST3::ParameterSupport::ParameterSupport(AudioPluginInstanceVST3* owner) : owner(owner) {
    auto controller = owner->controller;
    auto count = controller->vtable->controller.get_parameter_count(controller);

    parameter_ids = (v3_param_id*) calloc(sizeof(v3_param_id), count);

    for (auto i = 0; i < count; i++) {
        v3_param_info info{};
        controller->vtable->controller.get_parameter_info(controller, i, &info);
        std::string id = std::format("{}", info.param_id);
        std::string name = vst3StringToStdString(info.title);
        std::string path{""};

        auto p = new PluginParameter(id, name, path, info.default_normalised_value, 0, 1, true, info.flags & V3_PARAM_IS_HIDDEN);
        parameter_ids[i] = info.param_id;
        parameter_defs.emplace_back(p);
    }
}

remidy::AudioPluginInstanceVST3::ParameterSupport::~ParameterSupport() {
    for (auto p : parameter_defs)
        delete p;
    free(parameter_ids);
}

std::vector<remidy::PluginParameter*> remidy::AudioPluginInstanceVST3::ParameterSupport::parameters() {
    return parameter_defs;
}

remidy::StatusCode remidy::AudioPluginInstanceVST3::ParameterSupport::setParameter(uint32_t index, double value, uint64_t timestamp) {
    // FIXME: use IParamValueChanges for sample-accurate parameter changes
    auto controller = owner->controller;
    controller->vtable->controller.set_parameter_normalised(controller, parameter_ids[index], value);
    return StatusCode::OK;
}

remidy::StatusCode remidy::AudioPluginInstanceVST3::ParameterSupport::getParameter(uint32_t index, double* value) {
    if (!value)
        return StatusCode::INVALID_PARAMETER_OPERATION;
    auto controller = owner->controller;
    *value = controller->vtable->controller.get_parameter_normalised(controller, parameter_ids[index]);
    return StatusCode::OK;
}

// TypedUmpInputDispatcherVST3

void remidy::AudioPluginInstanceVST3::VST3UmpInputDispatcher::onNoteOn(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t note,
                                                                       uint8_t attributeType, uint16_t velocity, uint16_t attribute) {
    // use IEventList to add Event (kNoteOnEvent)
    auto& el = owner->processDataInputEvents;
    int32_t noteId = -1; // FIXME: create one
    v3_event_note_on noteOn{channel, note, 0, (float) (velocity / 65535.0), 0, noteId};
    v3_event e{group, static_cast<int32_t>(timestamp()), trackContext()->ppqPosition(), 0,
               v3_event_type::V3_EVENT_NOTE_ON, {.note_on = noteOn}};
    el.vtable->event_list.add_event(&el, &e);
}

void remidy::AudioPluginInstanceVST3::VST3UmpInputDispatcher::onNoteOff(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t note,
                                                                        uint8_t attributeType, uint16_t velocity, uint16_t attribute) {
    // use IEventList to add Event (kNoteOffEvent)
    auto& el = owner->processDataInputEvents;
    int32_t noteId = -1; // FIXME: create one
    v3_event_note_off noteOff{channel, note, (float) (velocity / 65535.0), noteId};
    v3_event e{group, static_cast<int32_t>(timestamp()), trackContext()->ppqPosition(), 0,
               v3_event_type::V3_EVENT_NOTE_ON, {.note_off = noteOff}};
    el.vtable->event_list.add_event(&el, &e);
}

void remidy::AudioPluginInstanceVST3::VST3UmpInputDispatcher::onAC(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t bank,
                                                                   remidy::uint7_t index, uint32_t data, bool relative) {
    // parameter change (index = bank * 128 + index)
}

void remidy::AudioPluginInstanceVST3::VST3UmpInputDispatcher::onPNAC(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t note,
                                                                     uint8_t index, uint32_t data) {
    // Per-note controller
}

void remidy::AudioPluginInstanceVST3::VST3UmpInputDispatcher::onCC(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t index,
                                                                   uint32_t data) {
    // parameter change, use IMidiMapping to resolve index
}

void remidy::AudioPluginInstanceVST3::VST3UmpInputDispatcher::onProgramChange(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t flags,
                                                                              remidy::uint7_t program, remidy::uint7_t bankMSB, remidy::uint7_t bankLSB) {
    // use IProgramListData to set program
}

void remidy::AudioPluginInstanceVST3::VST3UmpInputDispatcher::onRC(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t bank,
                                                                   remidy::uint7_t index, uint32_t data, bool relative) {
    // nothing is mapped here
}

void remidy::AudioPluginInstanceVST3::VST3UmpInputDispatcher::onPNRC(remidy::uint4_t group, remidy::uint4_t channel, remidy::uint7_t note,
                                                                     uint8_t index, uint32_t data) {
    // nothing is mapped here
}

void remidy::AudioPluginInstanceVST3::VST3UmpInputDispatcher::onPitchBend(remidy::uint4_t group, remidy::uint4_t channel, int8_t perNoteOrMinus, uint32_t data) {
    // use parameter kPitchBend (if available)
}

void remidy::AudioPluginInstanceVST3::VST3UmpInputDispatcher::onPressure(remidy::uint4_t group, remidy::uint4_t channel,
                                                                         int8_t perNoteOrMinus, uint32_t data) {
    // CAf: use parameter kAfterTouch (if available)
    // PAf: use IEventList to add Event (kPolyPressureEvent)
}

