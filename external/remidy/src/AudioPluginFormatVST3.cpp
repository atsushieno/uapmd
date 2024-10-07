
#include <iostream>

#include "remidy.hpp"

#include "vst3/VST3Helper.hpp"

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
        HostApplication host{};
        void scanAllAvailablePluginsFromLibrary(std::filesystem::path vst3Dir, std::vector<PluginClassInfo>& results);
        std::unique_ptr<PluginCatalogEntry> createPluginInformation(PluginClassInfo& info);

    public:
        explicit Impl(AudioPluginFormatVST3* owner) :
            owner(owner),
            logger(Logger::global()),
            extensibility(*owner),
            loadFunc([&](std::filesystem::path &vst3Dir, void** module)->StatusCode { return doLoad(vst3Dir, module); }),
            unloadFunc([&](std::filesystem::path &vst3Dir, void* module)->StatusCode { return doUnload(vst3Dir, module); }),
            library_pool(loadFunc,unloadFunc) {
        }

        AudioPluginExtensibility<AudioPluginFormat>* getExtensibility();
        PluginCatalog scanAllAvailablePlugins();
        void forEachPlugin(std::filesystem::path& vst3Dir,
            const std::function<void(void* module, IPluginFactory* factory, PluginClassInfo& info)>& func,
            const std::function<void(void* module)>& cleanup
        );
        void createInstance(PluginCatalogEntry *info, std::function<void(InvokeResult)> callback);
        void unrefLibrary(PluginCatalogEntry *info);
        PluginCatalog createCatalogFragment(const std::filesystem::path &bundlePath);
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
        auto idString = std::string{reinterpret_cast<char *>(info.tuid)};
        ret->bundlePath(info.bundlePath);
        ret->pluginId(idString);
        ret->setMetadataProperty(PluginCatalogEntry::MetadataPropertyID::DisplayName, info.name);
        ret->setMetadataProperty(PluginCatalogEntry::MetadataPropertyID::VendorName, info.vendor);
        ret->setMetadataProperty(PluginCatalogEntry::MetadataPropertyID::ProductUrl, info.url);
        return std::move(ret);
    }

    class AudioPluginInstanceVST3 : public AudioPluginInstance {
    public:
        StatusCode configure(int32_t sampleRate) override;

        StatusCode process(AudioProcessContext &process) override;

    private:
        AudioPluginFormatVST3::Impl* owner;
        PluginCatalogEntry* info;
        void* module;
        IComponent* component;
        IEditController* controller;
        bool isControllerDistinctFromComponent;
        FUnknown* instance;
    public:
        explicit AudioPluginInstanceVST3(
            AudioPluginFormatVST3::Impl* owner,
            PluginCatalogEntry* info,
            void* module,
            IComponent* component,
            IEditController* controller,
            bool isControllerDistinctFromComponent,
            FUnknown* instance
        ) : owner(owner), info(info), module(module), component(component), controller(controller), isControllerDistinctFromComponent(isControllerDistinctFromComponent), instance(instance) {
        }

        ~AudioPluginInstanceVST3() override {
            if (isControllerDistinctFromComponent) {
                controller->vtable->base.terminate(controller);
                controller->vtable->unknown.unref(controller);
            }
            component->vtable->base.terminate(component);
            component->vtable->unknown.unref(component);

            instance->vtable->unknown.unref(instance);

            owner->unrefLibrary(info);
        }
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

    PluginCatalog AudioPluginFormatVST3::scanAllAvailablePlugins() {
        return impl->scanAllAvailablePlugins();
    }

    std::string AudioPluginFormatVST3::savePluginInformation(AudioPluginInstance *instance) {
        // FIXME: implement
        throw std::runtime_error("savePluginInformation() is not implemented yet.");
    }

    std::unique_ptr<PluginCatalogEntry> AudioPluginFormatVST3::restorePluginInformation(std::string &data) {
        // FIXME: implement
        throw std::runtime_error("restorePluginInformation() is not implemented yet.");
    }

    PluginCatalog AudioPluginFormatVST3::Impl::createCatalogFragment(
        const std::filesystem::path &bundlePath) {
        if (strcasecmp(bundlePath.extension().c_str(), ".vst3") != 0)
            return PluginCatalog{};
        std::vector<PluginClassInfo> infos{};
        scanAllAvailablePluginsFromLibrary(bundlePath, infos);

        PluginCatalog ret{};
        for (auto& info : infos) {
            ret.add(createPluginInformation(info));
        }
        return ret;
    }

    std::string AudioPluginFormatVST3::savePluginInformation(PluginCatalogEntry *identifier) {
        return identifier->bundlePath();
    }

    PluginCatalog AudioPluginFormatVST3::Impl::scanAllAvailablePlugins() {
        PluginCatalog ret{};
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
            ret.add(createPluginInformation(info));
        return ret;
    }

    // AudioPluginInstanceVST3

    StatusCode AudioPluginInstanceVST3::configure(int32_t sampleRate) {
        throw std::runtime_error("AudioPluginInstanceVST3::configure() not implemented");
    }

    StatusCode AudioPluginInstanceVST3::process(AudioProcessContext &process) {
        throw std::runtime_error("AudioPluginInstanceVST3::process() not implemented");
    }

    void AudioPluginFormatVST3::createInstance(PluginCatalogEntry *info, std::function<void(InvokeResult)> callback) {
        return impl->createInstance(info, callback);
    }

    PluginCatalog AudioPluginFormatVST3::createCatalogFragment(std::filesystem::path &bundlePath) {
        return impl->createCatalogFragment(bundlePath);
    }

    void AudioPluginFormatVST3::Impl::createInstance(PluginCatalogEntry *pluginInfo, std::function<void(InvokeResult)> callback) {
        std::unique_ptr<AudioPluginInstanceVST3> ret{nullptr};
        v3_tuid tuid{};
        memcpy(&tuid, pluginInfo->pluginId().c_str(), sizeof(tuid));
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
                    ret = std::make_unique<AudioPluginInstanceVST3>(this, pluginInfo, module, component, controller, controllerDistinct, instance);
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
        // FIXME: try to load moduleinfo.json and skip loading dynamic library.

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
        forEachPlugin(vst3Dir, [&](void* module, IPluginFactory* factory, PluginClassInfo& pluginInfo) {
            results.emplace_back(pluginInfo);
        }, [&](void* module) {
            library_pool.removeReference(vst3Dir);
        });
    }
}
