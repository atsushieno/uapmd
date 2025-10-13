
#include "PluginFormatCLAP.hpp"
#include "CLAPHelper.hpp"
#include <format>

namespace remidy {
    // PluginFormatCLAP

    PluginFormatCLAP::PluginFormatCLAP(std::vector<std::string> &overrideSearchPaths) {
        impl = new Impl(this);
    }

    PluginFormatCLAP::~PluginFormatCLAP() {
        delete impl;
    }

    PluginScanning *PluginFormatCLAP::scanning() {
        return impl->scanning();
    }

    PluginExtensibility<PluginFormat> *PluginFormatCLAP::getExtensibility() {
        return impl->getExtensibility();
    }

    void PluginFormatCLAP::createInstance(
        PluginCatalogEntry* info,
        PluginInstantiationOptions options,
        std::function<void(std::unique_ptr<PluginInstance> instance, std::string error)> callback
    ) {
        impl->createInstance(info, callback);
    }

    PluginFormatCLAP::Extensibility::Extensibility(PluginFormat &format)
            : PluginExtensibility(format) {
    }

    // PluginScannerCLAP

    std::vector<std::filesystem::path> &PluginScannerCLAP::getDefaultSearchPaths() {
        static std::filesystem::path defaultSearchPathsCLAP[] = {
#if _WIN32
                std::string(getenv("COMMONPROGRAMFILES")) + "\\CLAP",
                std::string(getenv("LOCALAPPDATA")) + "\\Programs\\Common\\CLAP"
#elif __APPLE__
                "/Library/Audio/Plug-Ins/CLAP",
                std::string(getenv("HOME")) + "/Library/Audio/Plug-Ins/CLAP"
#else // We assume the rest covers Linux and other Unix-y platforms
                std::string(getenv("HOME")) + "/.clap",
                "/usr/lib/clap"
#endif
        };
        static std::vector<std::filesystem::path> ret = [] {
            std::vector<std::filesystem::path> paths{};
            for (auto &path: defaultSearchPathsCLAP)
                paths.emplace_back(path);
            return paths;
        }();
        return ret;
    }

    bool PluginScannerCLAP::usePluginSearchPaths() { return true; }

    PluginScanning::ScanningStrategyValue
    PluginScannerCLAP::scanRequiresLoadLibrary() { return ScanningStrategyValue::MAYBE; }

    PluginScanning::ScanningStrategyValue
    PluginScannerCLAP::scanRequiresInstantiation() { return ScanningStrategyValue::NEVER; }

    void PluginScannerCLAP::scanAllAvailablePluginsInPath(std::filesystem::path path, std::vector<std::unique_ptr<PluginCatalogEntry>>& entries) {
        std::filesystem::path dir{path};
        if (is_directory(dir)) {
            for (auto &entry: std::filesystem::directory_iterator(dir)) {
                if (!remidy_strcasecmp(entry.path().extension().string().c_str(), ".clap"))
                    scanAllAvailablePluginsFromLibrary(entry.path(), entries);
                else
                    scanAllAvailablePluginsInPath(entry.path(), entries);
            }
        }
    }

    std::vector<std::unique_ptr<PluginCatalogEntry>> PluginScannerCLAP::scanAllAvailablePlugins() {
        std::vector<std::unique_ptr<PluginCatalogEntry>> ret{};
        for (auto &path: getDefaultSearchPaths())
            scanAllAvailablePluginsInPath(path, ret);
        return ret;
    }

    void PluginScannerCLAP::scanAllAvailablePluginsFromLibrary(std::filesystem::path clapDir,
                                                                    std::vector<std::unique_ptr<PluginCatalogEntry>>& entries) {
        impl->getLogger()->logInfo("CLAP: scanning %s ", clapDir.c_str());

        if (isBlocklistedAsBundle(clapDir))
            return;

        impl->forEachPlugin(clapDir, [&](void *module, clap_plugin_factory_t* factory, clap_preset_discovery_factory* presetDiscoveryFactory, const clap_plugin_descriptor_t *descriptor) {
            auto e = std::make_unique<PluginCatalogEntry>();
            auto name = impl->owner->name();
            std::string id{descriptor->id};
            e->format(name);
            e->bundlePath(clapDir);
            e->displayName(descriptor->name);
            e->vendorName(descriptor->vendor);
            e->productUrl(descriptor->url);
            e->pluginId(id);

            entries.emplace_back(std::move(e));
        }, [&](void *module) {
            impl->libraryPool()->removeReference(clapDir);
        });
    }

    // PluginFormatCLAP::Impl

    // may return nullptr if it failed to load.
    void* loadModuleFromPath(std::filesystem::path clapPath) {
#if __APPLE__
        const auto allBundles = CFBundleGetAllBundles();
        CFBundleRef bundle{nullptr};
        for (size_t i = 0, n = CFArrayGetCount(allBundles); i < n; i++) {
            bundle = (CFBundleRef) CFArrayGetValueAtIndex(allBundles, i);
            const auto url = CFBundleCopyBundleURL(bundle);
            const auto pathString = CFURLCopyPath(url);
            if (!strcmp(CFStringGetCStringPtr(pathString, kCFStringEncodingUTF8), clapPath.c_str())) {
                // increase the reference count and return it
                CFRetain(bundle);
                CFRelease(pathString);
                CFRelease(url);
                return bundle;
            }
            CFRelease(pathString);
            CFRelease(url);
        }
        const auto filePath = CFStringCreateWithBytes(
            kCFAllocatorDefault,
            (const UInt8*) clapPath.c_str(),
            clapPath.string().size(),
            CFStringEncoding{},
            false);
        const auto cfUrl = CFURLCreateWithFileSystemPath(
            kCFAllocatorDefault,
            filePath,
            kCFURLPOSIXPathStyle,
            true);
        const auto ret = CFBundleCreate(kCFAllocatorDefault, cfUrl);
        CFRelease(cfUrl);
        CFRelease(filePath);
        return ret;
#else
        return loadLibraryFromBinary(clapPath);
#endif
    }

    StatusCode PluginFormatCLAP::Impl::doLoad(std::filesystem::path &clapPath, void **module) const {
        *module = loadModuleFromPath(clapPath);
        return *module == nullptr ? StatusCode::FAILED_TO_INSTANTIATE : StatusCode::OK;
    };

    StatusCode PluginFormatCLAP::Impl::doUnload(std::filesystem::path &clapPath, void *module) {
        auto entrypoint = remidy_clap::getFactoryFromLibrary(module);
        if (entrypoint && entrypoint->deinit)
            entrypoint->deinit();
#if _WIN32
        FreeLibrary((HMODULE) module);
#elif __APPLE__
        CFRelease(module);
#else
        dlclose(module);
#endif
        return StatusCode::OK;
    }

    PluginExtensibility<PluginFormat> *PluginFormatCLAP::Impl::getExtensibility() {
        return &extensibility;
    }

    void PluginFormatCLAP::Impl::forEachPlugin(std::filesystem::path& clapPath,
            const std::function<void(void* module, clap_plugin_factory_t* factory, clap_preset_discovery_factory* presetDiscoveryFactory, const clap_plugin_descriptor_t* info)>& func,
            const std::function<void(void* module)>& cleanup // it should unload the library only when it is not kept alive e.g. scanning plugins.
        ) {
        // JUCE seems to do this, not sure if it is required (not sure if this point is correct either).
        auto savedPath = std::filesystem::current_path();
        if (is_directory(clapPath))
            std::filesystem::current_path(clapPath);

        bool loadedAsNew;
        auto module = this->library_pool.loadOrAddReference(clapPath, &loadedAsNew);

        if (module) {
            auto entrypoint = remidy_clap::getFactoryFromLibrary(module);
            if (!entrypoint) {
                getLogger()->logError("clap_entry was not found in: %s", clapPath.c_str());
                cleanup(module);
                return;
            }
            if (loadedAsNew) {
                if (!entrypoint->init) {
                    getLogger()->logError("clap_entry.init() was not found in: %s", clapPath.c_str());
                    cleanup(module);
                    return;
                }
                if (!entrypoint->init(clapPath.string().c_str())) {
                    getLogger()->logError("clap_entry.init() returned false in: %s", clapPath.c_str());
                    cleanup(module);
                    return;
                }
            }
            if (!entrypoint->get_factory) {
                getLogger()->logError("clap_entry.get_factory() was not found in: %s", clapPath.c_str());
                cleanup(module);
                return;
            }
            auto factory = (clap_plugin_factory_t*) entrypoint->get_factory(CLAP_PLUGIN_FACTORY_ID);
            if (!factory) {
                getLogger()->logError("clap_entry.get_factory() returned nothing (plugin): %s", clapPath.c_str());
                cleanup(module);
                return;
            }
            auto presetsDiscoveryFactory = (clap_preset_discovery_factory*) entrypoint->get_factory(CLAP_PRESET_DISCOVERY_FACTORY_ID);
            // it can be empty.

            for (size_t i = 0, n = factory->get_plugin_count(factory); i < n; i++) {
                const auto desc = factory->get_plugin_descriptor(factory, i);
                func(module, factory, presetsDiscoveryFactory, desc);
            }

            cleanup(module);
        }
        else
            logger->logError("Could not load the library from bundle: %s", clapPath.c_str());

        std::filesystem::current_path(savedPath);
    }

    void PluginFormatCLAP::Impl::createInstance(
        PluginCatalogEntry* info,
        std::function<void(std::unique_ptr<PluginInstance> instance, std::string error)> callback
    ) {
        std::unique_ptr<PluginInstanceCLAP> ret{nullptr};
        std::string error{};
        auto bundle = info->bundlePath();
        forEachPlugin(bundle, [info, this, &ret](void *module, clap_plugin_factory_t *factory, clap_preset_discovery_factory* presetDiscoveryFactory,
                                                       const clap_plugin_descriptor_t *desc) {
            if (info->pluginId() != desc->id)
                return;

            auto plugin = factory->create_plugin(factory, host.clapHost(), desc->id);
            if (plugin)
                ret = std::make_unique<PluginInstanceCLAP>(this, info, presetDiscoveryFactory, module, plugin);
        }, [&](void *module) {
            // do not unload library here.
        });
        if (ret)
            callback(std::move(ret), error);
        else
            callback(nullptr, std::format("Specified CLAP plugin {} could not be instantiated: {}", info->displayName(), error));
    }
}