
#include "PluginFormatCLAP.hpp"
#include "CLAPHelper.hpp"
#include "../utils.hpp"
#include <format>

#if _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#elif __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#elif defined(__linux__)
#include <dlfcn.h>
#endif

namespace remidy {
    // PluginFormatCLAP

    std::unique_ptr<PluginFormatCLAP> PluginFormatCLAP::create(std::vector<std::string>& overrideSearchPaths) {
        return std::make_unique<PluginFormatCLAPImpl>(overrideSearchPaths);
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

    bool PluginScannerCLAP::scanRequiresLoadLibrary(const std::filesystem::path& /*bundlePath*/) {
        return true;
    }

    PluginScanning::ScanningStrategyValue
    PluginScannerCLAP::scanRequiresInstantiation() { return ScanningStrategyValue::NEVER; }

    void PluginScannerCLAP::scanAllAvailablePluginsInPath(std::filesystem::path path, bool requireFastScanning) {
        std::filesystem::path dir{path};
        if (is_directory(dir)) {
            for (auto &entry: std::filesystem::directory_iterator(dir)) {
                if (!remidy_strcasecmp(entry.path().extension().string().c_str(), ".clap")) {
                    if (!requireFastScanning)
                        pendingSlowBundles_.emplace_back(entry.path());
                } else
                    scanAllAvailablePluginsInPath(entry.path(), requireFastScanning);
            }
        }
    }

std::vector<PluginCatalogEntry> PluginScannerCLAP::getAllFastScannablePlugins() {
        pendingSlowBundles_.clear();
        for (auto &path: getDefaultSearchPaths())
            scanAllAvailablePluginsInPath(path, false);
        for (auto& overridePath : getOverrideSearchPaths())
            scanAllAvailablePluginsInPath(std::filesystem::path{overridePath}, false);
        return {};
    }

    void PluginScannerCLAP::startSlowPluginScan(std::function<void(PluginCatalogEntry entry)> pluginFound,
                                                PluginScanCompletedCallback scanCompleted) {
        for (const auto& bundlePath : enumerateCandidateBundles(false)) {
            bool bundleCompleted = false;
            std::string bundleError;
            scanBundle(bundlePath, false, 0.0, pluginFound,
                       [&](std::string error) {
                           bundleError = std::move(error);
                           bundleCompleted = true;
                       });
            if (!bundleCompleted) {
                if (scanCompleted)
                    scanCompleted(std::format("CLAP slow scan for '{}' did not invoke completion.", bundlePath.string()));
                return;
            }
            if (!bundleError.empty()) {
                if (scanCompleted)
                    scanCompleted(std::move(bundleError));
                return;
            }
        }
        if (scanCompleted)
            scanCompleted("");
    }

    void PluginScannerCLAP::scanBundle(const std::filesystem::path& bundlePath,
                                       bool /*requireFastScanning*/,
                                       double /*timeoutSeconds*/,
                                       std::function<void(PluginCatalogEntry entry)> pluginFound,
                                       PluginScanCompletedCallback scanCompleted) {
        std::vector<PluginCatalogEntry> ret{};
        try {
            scanAllAvailablePluginsFromLibrary(bundlePath, ret);
            for (auto& entry : ret)
                if (pluginFound)
                    pluginFound(std::move(entry));
            if (scanCompleted)
                scanCompleted("");
        } catch (const std::exception& e) {
            if (scanCompleted)
                scanCompleted(e.what());
        } catch (...) {
            if (scanCompleted)
                scanCompleted("CLAP bundle scan failed.");
        }
    }


    void PluginScannerCLAP::scanAllAvailablePluginsFromLibrary(std::filesystem::path clapDir,
                                                                    std::vector<PluginCatalogEntry>& entries) {
        impl->getLogger()->logInfo("CLAP: scanning %s ", clapDir.c_str());

        if (isBlocklistedAsBundle(clapDir))
            return;

        impl->forEachPlugin(clapDir, [&](void *module, clap_plugin_factory_t* factory, clap_preset_discovery_factory* presetDiscoveryFactory, const clap_plugin_descriptor_t *descriptor) {
            PluginCatalogEntry e{};
            auto name = impl->name();
            std::string id{descriptor->id};
            e.format(name);
            e.bundlePath(clapDir);
            e.displayName(descriptor->name);
            e.vendorName(descriptor->vendor);
            e.productUrl(descriptor->url);
            e.pluginId(id);

            entries.emplace_back(std::move(e));
        }, [&](void *module) {
            impl->libraryPool()->removeReference(clapDir);
        });
    }

    namespace {
    void collectClapBundles(const std::filesystem::path& root,
                            std::vector<std::filesystem::path>& bundles) {
        std::error_code ec;
        if (!std::filesystem::exists(root, ec) || !std::filesystem::is_directory(root, ec))
            return;
        for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
            if (!entry.is_directory(ec))
                continue;
            if (!remidy_strcasecmp(entry.path().extension().string().c_str(), ".clap"))
                bundles.emplace_back(entry.path());
            else
                collectClapBundles(entry.path(), bundles);
        }
    }
    } // namespace

    std::vector<std::filesystem::path> PluginScannerCLAP::enumerateCandidateBundles(bool /*requireFastScanning*/) {
        std::vector<std::filesystem::path> bundles;
        if (!pendingSlowBundles_.empty()) {
            bundles = pendingSlowBundles_;
        } else {
            auto addRoot = [&](const std::filesystem::path& path) {
                collectClapBundles(path, bundles);
            };
            for (auto& path : getDefaultSearchPaths())
                addRoot(path);
            for (auto& path : getOverrideSearchPaths())
                addRoot(std::filesystem::path{path});
        }
        std::sort(bundles.begin(), bundles.end());
        bundles.erase(std::unique(bundles.begin(), bundles.end()), bundles.end());
        return bundles;
    }

    // PluginFormatCLAPImpl

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

    StatusCode PluginFormatCLAPImpl::doLoad(std::filesystem::path &clapPath, void **module) const {
        *module = loadModuleFromPath(clapPath);
        return *module == nullptr ? StatusCode::FAILED_TO_INSTANTIATE : StatusCode::OK;
    };

    StatusCode PluginFormatCLAPImpl::doUnload(std::filesystem::path &clapPath, void *module) {
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

    PluginExtensibility<PluginFormat> *PluginFormatCLAPImpl::getExtensibility() {
        return &extensibility;
    }

    void PluginFormatCLAPImpl::forEachPlugin(std::filesystem::path& clapPath,
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

    void PluginFormatCLAPImpl::createInstance(
        PluginCatalogEntry* info,
        PluginInstantiationOptions options,
        std::function<void(std::unique_ptr<PluginInstance> instance, std::string error)> callback
    ) {
        std::unique_ptr<PluginInstanceCLAP> ret{nullptr};
        std::string error{};
        auto bundle = info->bundlePath();
        forEachPlugin(bundle, [info, this, &ret](void *module, clap_plugin_factory_t *factory, clap_preset_discovery_factory* presetDiscoveryFactory,
                                                       const clap_plugin_descriptor_t *desc) {
            if (info->pluginId() != desc->id)
                return;

            auto host = std::make_unique<RemidyCLAPHost>();
            auto plugin = factory->create_plugin(factory, host->clapHost(), desc->id);
            if (plugin) {
                // Initialize the plugin via the proxy
                // Create the plugin proxy wrapper
                auto pluginProxy = std::make_unique<CLAPPluginProxy>(*plugin, *host);

                if (!pluginProxy->init()) {
                    Logger::global()->logError("Failed to initialize CLAP plugin %s", info->displayName().data());
                    return;
                }

                ret = std::make_unique<PluginInstanceCLAP>(this, info, presetDiscoveryFactory, module, std::move(pluginProxy), std::move(host));
            }
        }, [&](void *module) {
            // do not unload library here.
        });
        if (ret)
            callback(std::move(ret), error);
        else
            callback(nullptr, std::format("Specified CLAP plugin {} could not be instantiated: {}", info->displayName(), error));
    }
}
