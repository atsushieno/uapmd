
#include <iostream>

#include "remidy.hpp"
#include "../utils.hpp"

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
                                          PluginInstantiationOptions options,
                                          std::function<void(std::unique_ptr<PluginInstance> instance,
                                                             std::string error)> callback) {
        return impl->createInstance(info, callback);
    }

    void PluginFormatVST3::Impl::createInstance(PluginCatalogEntry *pluginInfo,
                                                std::function<void(std::unique_ptr<PluginInstance> instance,
                                                                   std::string error)> callback) {
        PluginCatalogEntry *entry = pluginInfo;
        std::unique_ptr<PluginInstanceVST3> ret{nullptr};
        std::string error{};
        TUID tuid{};
        auto decodedBytes = stringToHexBinary(entry->pluginId());
        memcpy(&tuid, decodedBytes.c_str(), decodedBytes.size());
        std::string name = entry->displayName();

        auto bundle = entry->bundlePath();
        forEachPlugin(bundle, [entry, &ret, tuid, name, &error, this](void *module, IPluginFactory *factory,
                                                                      PluginClassInfo &info) {
            if (memcmp(info.tuid, tuid, sizeof(TUID)) != 0)
                return;
            IPluginFactory3 *factory3{nullptr};
            auto result = factory->queryInterface(IPluginFactory3::iid, (void **) &factory3);
            if (result == kResultOk) {
                result = factory3->setHostContext((FUnknown *) &host);
                if (result != kResultOk) {
                    // It seems common that a plugin often "implements IPluginFactory3" and then returns kNotImplemented...
                    // In that case, it is not callable anyway, so treat it as if IPluginFactory3 were not queryable.
                    factory3->release();
                    if (((Extensibility *) getExtensibility())->reportNotImplemented())
                        logger->logWarning("Failed to set HostApplication to IPluginFactory3: %s result: %d",
                                           name.c_str(), result);
                    if (result != kNotImplemented)
                        return;
                }
            }

            FUnknown *instance{};
            result = factory->createInstance(tuid, IComponent::iid, (void **) &instance);
            if (result)
                return;

            IComponent *component{};
            if (!instance) {
                logger->logError("Invalid component instance returned for %s", name.c_str());
                if (instance)
                    instance->release();
                error = "Invalid component instance";
                return;
            }
            result = instance->queryInterface(IComponent::iid, (void **) &component);
            if (result != kResultOk || component == nullptr) {
                logger->logError("Failed to query VST3 component: %s result: %d", name.c_str(), result);
                instance->release();
                error = "Failed to query VST3 component";
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
            result = component->vtable->component.set_io_mode(instance, IoModes::kAdvanced);
            if (result != kResultOk && result != kNotImplemented) {
                logger->logError("Failed to set vst3 I/O mode: %s", name.c_str());
                component->vtable->unknown.unref(component);
                instance->vtable->unknown.unref(instance);
                return;
            }
#endif

            IAudioProcessor *processor{};
            if (!component) {
                logger->logError("Invalid component for %s", name.c_str());
                component->release();
                instance->release();
                error = "Invalid component";
                return;
            }
            result = component->queryInterface(IAudioProcessor::iid, (void **) &processor);
            if (result != kResultOk || processor == nullptr) {
                logger->logError("Could not query vst3 IAudioProcessor interface: %s (status: %d ", name.c_str(),
                                 result);
                error = "Could not query vst3 IAudioProcessor interface";
                component->release();
                instance->release();
                return;
            }

            // If we can query IEditController from the component, just use it.
            // Otherwise, the controller *instance* is different.
            bool controllerValid = false;
            bool distinctControllerInstance = false;
            IEditController *controller{nullptr};
            result = component->queryInterface(IEditController::iid, (void **) &controller);
            if (result == kResultOk && controller)
                controllerValid = true;
            else {
                controller = nullptr; // just to make sure

                // > Steinberg::Vst::IComponent::getControllerClassId can also be called before (See VST 3 Workflow Diagrams).
                // ... is it another "sole exception" ?
                TUID controllerClassId{};
                result = component->getControllerClassId(controllerClassId);
                if (result == kResultOk && memcmp(tuid, controllerClassId, sizeof(TUID)) != 0)
                    distinctControllerInstance = true;
                else
                    // result may be kNotImplemented, meaning that the controller IID is the same as component.
                    memcpy(controllerClassId, tuid, sizeof(TUID));

                // Create instance for IEditController.
                result = factory->createInstance(controllerClassId, IEditController::iid, (void **) &controller);
                if (result == kResultOk)
                    controllerValid = controller != nullptr;
            }

            // Now initialize the component and the controller.
            // (At this state I don't think it is realistic to only instantiate component without controller;
            // too many operations depend on it e.g. setting parameters)
            if (controllerValid)
                result = component->initialize((FUnknown *) &host);
            if (!controllerValid || result != kResultOk) {
                logger->logError("Failed to initialize vst3: %s (status: %d ", name.c_str(), result);
                error = "Failed to initialize vst3";
                component->release();
                instance->release();
                return;
            }
            if (distinctControllerInstance) {
                result = controller->initialize((FUnknown *) &host);
                if (result != kResultOk || controller == nullptr)
                    controllerValid = false;
            }
            if (controllerValid) {
                auto handler = host.getComponentHandler();
                result = controller->setComponentHandler((IComponentHandler *) handler);
                if (result == kResultOk) {
                    ret = std::make_unique<PluginInstanceVST3>(this, entry, module, factory, component, processor,
                                                                    controller, distinctControllerInstance, instance);
                    return;
                }
                error = "Failed to set VST3 component handler";
            } else
                error = "Failed to find or create valid VST3 controller";
            if (controller)
                controller->release();
            error = "Failed to instantiate VST3";
            component->terminate();
            // regardless of the result, we go on...

            component->release();
            instance->release();
        }, [&](void *module) {
            // do not unload library here.
        });
        if (ret)
            callback(std::move(ret), error);
        else
            callback(nullptr, std::format("Specified VST3 plugin {} could not be instantiated: {}", entry->displayName(), error));
    }

    void PluginFormatVST3::Impl::unrefLibrary(PluginCatalogEntry *info) {
        library_pool.removeReference(info->bundlePath());
    }

    // PluginScannerVST3

    std::vector<std::filesystem::path> &PluginScannerVST3::getDefaultSearchPaths() {
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

    std::unique_ptr<PluginCatalogEntry> PluginScannerVST3::createPluginInformation(PluginClassInfo &info) {
        auto ret = std::make_unique<PluginCatalogEntry>();
        static std::string format{"VST3"};
        ret->format(format);
        auto idString = hexBinaryToString((char *) info.tuid, sizeof(TUID));
        ret->bundlePath(info.bundlePath);
        ret->pluginId(idString);
        ret->displayName(info.name);
        ret->vendorName(info.vendor);
        ret->productUrl(info.url);
        return ret;
    }

    bool PluginScannerVST3::usePluginSearchPaths() { return true; }

    PluginScanning::ScanningStrategyValue
    PluginScannerVST3::scanRequiresLoadLibrary() { return ScanningStrategyValue::MAYBE; }

    PluginScanning::ScanningStrategyValue
    PluginScannerVST3::scanRequiresInstantiation() { return ScanningStrategyValue::ALWAYS; }

    void PluginScannerVST3::scanAllAvailablePluginsInPath(std::filesystem::path path, std::vector<PluginClassInfo>& infos) {
        std::filesystem::path dir{path};
        if (is_directory(dir)) {
            for (auto &entry: std::filesystem::directory_iterator(dir)) {
                if (!remidy_strcasecmp(entry.path().extension().string().c_str(), ".vst3"))
                    scanAllAvailablePluginsFromLibrary(entry.path(), infos);
                else
                    scanAllAvailablePluginsInPath(entry.path(), infos);
            }
        }
    }

    std::vector<std::unique_ptr<PluginCatalogEntry>> PluginScannerVST3::scanAllAvailablePlugins() {
        std::vector<PluginClassInfo> infos;
        for (auto &path: getDefaultSearchPaths())
            scanAllAvailablePluginsInPath(path, infos);
        std::vector<std::unique_ptr<PluginCatalogEntry>> ret{};
        for (auto &info: infos)
            ret.emplace_back(createPluginInformation(info));
        return ret;
    }

    void PluginScannerVST3::scanAllAvailablePluginsFromLibrary(std::filesystem::path vst3Dir,
                                                                    std::vector<PluginClassInfo> &results) {
        impl->getLogger()->logInfo("VST3: scanning %s ", vst3Dir.c_str());
        // fast path scanning using moduleinfo.json
        if (remidy_vst3::hasModuleInfo(vst3Dir)) {
            for (auto &e: remidy_vst3::getModuleInfo(vst3Dir))
                results.emplace_back(e);
            return;
        }

        if (isBlocklistedAsBundle(vst3Dir))
            return;

        impl->forEachPlugin(vst3Dir, [&](void *module, IPluginFactory *factory, PluginClassInfo &pluginInfo) {
            results.emplace_back(pluginInfo);
        }, [&](void *module) {
            impl->libraryPool()->removeReference(vst3Dir);
        });
    }

    // Loader helpers

    void PluginFormatVST3::Impl::forEachPlugin(std::filesystem::path &vst3Path,
                                               const std::function<void(void *module, IPluginFactory *factory,
                                                                        PluginClassInfo &info)> &func,
                                               const std::function<void(void *module)> &cleanup
    ) {
        // JUCE seems to do this, not sure if it is required (not sure if this point is correct either).
        auto savedPath = std::filesystem::current_path();
        if (is_directory(vst3Path))
            std::filesystem::current_path(vst3Path);

        bool loadedAsNew;
        auto module = this->library_pool.loadOrAddReference(vst3Path, &loadedAsNew);

        if (module) {
            auto factory = getFactoryFromLibrary(module);
            if (!factory) {
                std::filesystem::current_path(savedPath);
                return;
            }

            // FIXME: we need to retrieve classInfo2, classInfo3, ...
            PFactoryInfo fInfo{};
            factory->getFactoryInfo(&fInfo);
            for (int i = 0, n = factory->countClasses(); i < n; i++) {
                PClassInfo cls{};
                auto result = factory->getClassInfo(i, &cls);
                if (result == 0) {
                    if (!strcmp(cls.category, kVstAudioEffectClass)) {
                        std::string name = std::string{cls.name}.substr(0, strlen(cls.name));
                        std::string vendor = std::string{fInfo.vendor}.substr(0, strlen(fInfo.vendor));
                        std::string url = std::string{fInfo.url}.substr(0, strlen(fInfo.url));
                        PluginClassInfo info(vst3Path, vendor, url, name, cls.cid);
                        func(module, factory, info);
                    }
                } else
                    logger->logError("failed to retrieve class info at %d, in %s", i, vst3Path.c_str());
            }
            cleanup(module);
        } else
            logger->logError("Could not load the library from bundle: %s", vst3Path.c_str());

        std::filesystem::current_path(savedPath);
    }
}
