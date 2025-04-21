
#include <iostream>

#include "remidy.hpp"
#include "../utils.hpp"

#include "PluginFormatVST3.hpp"

#ifdef _MSC_VER
#define strcasecmp _wcsicmp
#endif

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

    PluginScanning *PluginFormatVST3::scanning() {
        return impl->scanning();
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

    PluginScanning::ScanningStrategyValue
    AudioPluginScannerVST3::scanRequiresLoadLibrary() { return ScanningStrategyValue::MAYBE; }

    PluginScanning::ScanningStrategyValue
    AudioPluginScannerVST3::scanRequiresInstantiation() { return ScanningStrategyValue::ALWAYS; }

    void AudioPluginScannerVST3::scanAllAvailablePluginsInPath(std::filesystem::path path, std::vector<PluginClassInfo>& infos) {
        std::filesystem::path dir{path};
        if (is_directory(dir)) {
            for (auto &entry: std::filesystem::directory_iterator(dir)) {
#if WIN32
                if (!strcasecmp(entry.path().extension().c_str(), L".vst3"))
#else
                if (!strcasecmp(entry.path().extension().c_str(), ".vst3"))
#endif
                    scanAllAvailablePluginsFromLibrary(entry.path(), infos);
                else
                    scanAllAvailablePluginsInPath(entry.path(), infos);
            }
        }
    }

    std::vector<std::unique_ptr<PluginCatalogEntry>> AudioPluginScannerVST3::scanAllAvailablePlugins() {
        std::vector<PluginClassInfo> infos;
        for (auto &path: getDefaultSearchPaths())
            scanAllAvailablePluginsInPath(path, infos);
        std::vector<std::unique_ptr<PluginCatalogEntry>> ret{};
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
}
