
#include <cassert>
#include <dlfcn.h>
#include <iostream>
#include "VST3Helper.hpp"
#include "AudioPluginFormatVST3.hpp"

namespace remidy {

    class AudioPluginIdentifierVST3 : public AudioPluginIdentifier {
    private:
        std::string idString{};
    public:
        std::string & getVendor() override;

        std::string & getUrl() override;

        std::string& getUniqueId() override;

        std::string& getDisplayName() override;

    private:
    public:
        PluginClassInfo info;

        explicit AudioPluginIdentifierVST3(PluginClassInfo& info) : info(info) {
            idString = reinterpret_cast<char *>(info.tuid);
        }
    };

    std::string & AudioPluginIdentifierVST3::getVendor() { return info.vendor; }
    std::string & AudioPluginIdentifierVST3::getUrl() { return info.url; }
    std::string& AudioPluginIdentifierVST3::getUniqueId() { return idString; }
    std::string & AudioPluginIdentifierVST3::getDisplayName() { return info.className; }

    std::function<remidy_status_t(std::filesystem::path &vst3Dir, void **module)> loadFunc =
            [](std::filesystem::path &vst3Dir, void** module) {
        *module = loadLibraryFromBundle(vst3Dir);
        if (*module) {
            auto err = initializeModule(*module);
            if (err != 0) {
                std::cerr << "Could not initialize the module from bundle: " << vst3Dir.c_str() << std::endl;
                unloadLibrary(*module);
                *module = nullptr;
            }
        }

        // FIXME: define status codes
        return *module == nullptr;
    };
    std::function<remidy_status_t(std::filesystem::path &libraryFile, void* module)> unloadFunc =
            [](std::filesystem::path &libraryFile, void* module) {
                unloadLibrary(module);
                // FIXME: define status codes
                return 0;
    };

    class AudioPluginFormatVST3::Impl {
        AudioPluginFormatVST3* owner;
        AudioPluginLibraryPool library_pool;
        HostApplication host{};
        void scanAllAvailablePluginsFromLibrary(std::filesystem::path vst3Dir, std::vector<PluginClassInfo>& results);

    public:
        explicit Impl(AudioPluginFormatVST3* owner) :
            owner(owner),
            library_pool(loadFunc,unloadFunc) {
        }

        std::vector<std::unique_ptr<AudioPluginIdentifierVST3>> plugin_list_cache{};
        void scanAllAvailablePlugins();
        void forEachPlugin(std::filesystem::path vst3Dir,
            std::function<void(void* module, IPluginFactory* factory, PluginClassInfo& info)> func,
            std::function<void(void* module)> cleanup
        );
        AudioPluginInstance* createInstance(AudioPluginIdentifier *uniqueId);
        void removeInstance(AudioPluginIdentifierVST3* vst3Id);
    };

    std::vector<std::string>& AudioPluginFormatVST3::getDefaultSearchPaths() {
        static std::string defaultSearchPathsVST3[] = {
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
        static std::vector<std::string> ret = [] {
            std::vector<std::string> paths{};
            paths.append_range(defaultSearchPathsVST3);
            return paths;
        }();
        return ret;
    }

    AudioPluginFormatVST3::AudioPluginFormatVST3(std::vector<std::string> &overrideSearchPaths)
        : DesktopAudioPluginFormat() {
        impl = new Impl(this);
    }
    AudioPluginFormatVST3::~AudioPluginFormatVST3() {
        delete impl;
    }


    bool AudioPluginFormatVST3::usePluginSearchPaths() { return true;}

    AudioPluginFormat::ScanningStrategyValue AudioPluginFormatVST3::scanRequiresLoadLibrary() { return YES; }

    AudioPluginFormat::ScanningStrategyValue AudioPluginFormatVST3::scanRequiresInstantiation() { return MAYBE; }

    std::vector<AudioPluginIdentifier*> AudioPluginFormatVST3::scanAllAvailablePlugins() {
        std::vector<AudioPluginIdentifier*> ret{};
        impl->scanAllAvailablePlugins();
        for (auto& id : impl->plugin_list_cache)
            ret.emplace_back(id.get());
        return ret;
    }

    void AudioPluginFormatVST3::Impl::scanAllAvailablePlugins() {
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
        for (auto &id : plugin_list_cache)
            id.reset();
        plugin_list_cache.clear();
        for (auto &info : infos)
            plugin_list_cache.emplace_back(std::make_unique<AudioPluginIdentifierVST3>(info));
    }

    class AudioPluginInstanceVST3 : public AudioPluginInstance {
    public:
        remidy_status_t configure(int32_t sampleRate) override;

        remidy_status_t process(AudioProcessContext &process) override;

    private:
        AudioPluginFormatVST3::Impl* owner;
        AudioPluginIdentifierVST3* identifier;
        void* module;
        IComponent* component;
        IEditController* controller;
        bool isControllerDistinctFromComponent;
        FUnknown* instance;
    public:
        explicit AudioPluginInstanceVST3(
            AudioPluginFormatVST3::Impl* owner,
            AudioPluginIdentifierVST3* identifier,
            void* module,
            IComponent* component,
            IEditController* controller,
            bool isControllerDistinctFromComponent,
            FUnknown* instance
        ) : owner(owner), identifier(identifier), module(module), component(component), controller(controller), isControllerDistinctFromComponent(isControllerDistinctFromComponent), instance(instance) {
        }

        ~AudioPluginInstanceVST3() override {
            if (isControllerDistinctFromComponent) {
                controller->vtable->base.terminate(controller);
                controller->vtable->unknown.unref(controller);
            }
            component->vtable->base.terminate(component);
            component->vtable->unknown.unref(component);

            owner->removeInstance(identifier);
        }
    };

    remidy_status_t AudioPluginInstanceVST3::configure(int32_t sampleRate) {
        throw std::runtime_error("AudioPluginInstanceVST3::configure() not implemented");
    }

    remidy_status_t AudioPluginInstanceVST3::process(AudioProcessContext &process) {
        throw std::runtime_error("AudioPluginInstanceVST3::process() not implemented");
    }

    AudioPluginInstance* AudioPluginFormatVST3::createInstance(AudioPluginIdentifier *uniqueId) {
        return impl->createInstance(uniqueId);
    }

    AudioPluginInstance * AudioPluginFormatVST3::Impl::createInstance(AudioPluginIdentifier *uniqueId) {
        auto vst3Id = (AudioPluginIdentifierVST3*) uniqueId;
        AudioPluginInstanceVST3* ret{nullptr};

        forEachPlugin(vst3Id->info.bundlePath, [&](void* module, IPluginFactory* factory, PluginClassInfo &info) {
            if (memcmp(info.tuid, vst3Id->info.tuid, sizeof(v3_tuid)) != 0)
                return;

            IPluginFactory3* factory3{nullptr};
            auto result = factory->vtable->unknown.query_interface(factory, v3_plugin_factory_3_iid, (void**) &factory3);
            if (result == V3_OK) {
                result = factory3->vtable->factory_3.set_host_context(factory3, (v3_funknown**) &host);
                // It seems common that a plugin often "implements IPluginFactory3" and then returns kNotImplemented...
                // In that case, it is not callable anyway, so treat it as if IPluginFactory3 were not queryable.
                factory3->vtable->unknown.unref(factory3);
                if (result != V3_OK) {
                    std::cerr << "Failed to set HostApplication to IPluginFactory3: " << uniqueId->getDisplayName() << " result: " << result << std::endl;
                    if (result != V3_NOT_IMPLEMENTED)
                        return;
                }
            }

            FUnknown* instance{};
            result = factory->vtable->factory.create_instance(factory, vst3Id->info.tuid, v3_component_iid, (void**) &instance);
            if (result) // not about this class
                return;

            IComponent *component{};
            result = instance->vtable->unknown.query_interface(instance, v3_component_iid, (void**) &component);
            if (result != V3_OK) {
                std::cerr << "Failed to query VST3 component: " << uniqueId->getDisplayName() << " result: " << result << std::endl;
                return;
            }

            // From https://steinbergmedia.github.io/vst3_dev_portal/pages/Technical+Documentation/API+Documentation/Index.html#initialization :
            // > Hosts should not call other functions before initialize is called, with the sole exception of Steinberg::Vst::IComponent::setIoMode
            // > which must be called before initialize.
            result = component->vtable->component.set_io_mode(instance, V3_IO_ADVANCED);
            if (result != V3_OK && result != V3_NOT_IMPLEMENTED) {
                std::cerr << "Failed to set vst3 I/O mode: " << uniqueId->getDisplayName() << std::endl;
                component->vtable->unknown.unref(component);
                return;
            }

            // If we can instantiate controller from the component, just use it.
            bool controllerDistinct = false;
            IEditController* controller{nullptr};
            bool controllerValid = false;
            result = component->vtable->unknown.query_interface(component, v3_edit_controller_iid, (void**) &controller);
            if (result == V3_OK)
                controllerValid = true;

            // Now initialize the component, and optionally initialize the controller.
            result = component->vtable->base.initialize(component, (v3_funknown**) &host);
            if (result == V3_OK) {
                // > Steinberg::Vst::IComponent::getControllerClassId can also be called before (See VST 3 Workflow Diagrams).
                // ... is it another "sole exception" ?
                v3_tuid controllerClassId{};
                result = component->vtable->component.get_controller_class_id(instance, controllerClassId);
                if (result == V3_OK && memcmp(vst3Id->info.tuid, controllerClassId, sizeof(v3_tuid)) != 0) {
                    controllerDistinct = true;
                    result = factory->vtable->factory.create_instance(factory, controllerClassId, v3_edit_controller_iid, (void**) &controller);
                    if (result == V3_OK) {
                        result = controller->vtable->base.initialize(controller, (v3_funknown**) &host);
                        if (result == V3_OK)
                            controllerValid = true;
                    }
                }
            }
            if (controllerValid) {
                auto handler = host.getComponentHandler();
                result = controller->vtable->controller.set_component_handler(controller, (v3_component_handler**) handler);
                if (result == V3_OK) {
                    ret = new AudioPluginInstanceVST3(this, vst3Id, module, component, controller, controllerDistinct, instance);
                    return;
                }
            }
            if (controller)
                controller->vtable->unknown.unref(controller);
            std::cerr << "Failed to initialize vst3: " << uniqueId->getDisplayName() << std::endl;
            component->vtable->unknown.unref(component);
        }, [&](void* module) {
            // do not unload library here.
        });
        return ret;
    }

    void AudioPluginFormatVST3::Impl::removeInstance(AudioPluginIdentifierVST3* vst3Id) {
        // FIXME: this should be enabled (but we might need decent refcounted module handler
        library_pool.removeReference(vst3Id->info.bundlePath);
    }

    // Loader helpers

    std::filesystem::path getPluginCodeFile(std::filesystem::path& pluginPath) {
        // https://steinbergmedia.github.io/vst3_dev_portal/pages/Technical+Documentation/Locations+Format/Plugin+Format.html
#if _WIN32
#if __x86_64__
        auto abiDirName = "x86_64-win"
#elif __x86_32__
        auto abiDirName = "x86_32-win"
#elif __aarch64__
        // FIXME: there are also arm64-win and arm64x-win
        auto abiDirName = "arm64ec-win";
#else // at this state we assume the only remaining platform is arm32.
        auto abiDirName = "arm32-win";
#endif
        auto binDir = pluginPath.append("Contents").append(abiDirName);
#elif __APPLE__
        auto binDir = pluginPath.append("Contents").append("MacOS");
#else
#if __x86_64__
        auto binDir = pluginPath.append("Contents").append("x86_64-linux");
#else
        auto binDir = pluginPath.append("Contents").append("i386-linux");
#endif
#endif
        for (auto& entry : std::filesystem::directory_iterator(binDir))
            return entry.path();
        return {};
    }

    void* loadLibraryFromBundle(std::filesystem::path vst3Dir) {
#if __APPLE__
        auto u8 = (const UInt8*) vst3Dir.c_str();
        auto ret = CFBundleCreate(kCFAllocatorDefault,
            CFURLCreateWithFileSystemPath(kCFAllocatorDefault,
                CFStringCreateWithBytes(kCFAllocatorDefault, u8, vst3Dir.string().size(), CFStringEncoding{}, false),
                kCFURLPOSIXPathStyle, true));
        assert(ret);
        return ret;
#else
        const auto libraryFilePath = getPluginCodeFile(vst3Dir);
        if (!libraryFilePath.empty()) {
            const auto library = loadLibraryFromBinary(libraryFilePath);
        }
        else
            return nullptr;
#endif
    }

#if __APPLE__
    CFStringRef createCFString(const char* s) {
        return CFStringCreateWithBytes(kCFAllocatorDefault, (UInt8*) s, strlen(s), CFStringEncoding{}, false);
    }
#endif

    // The returned library (platform dependent) must be released later (in the platform manner)
    // It might fail due to ABI mismatch on macOS. We have to ignore the error and return NULL.
    void* loadLibraryFromBinary(std::filesystem::path libraryFile) {
#if _WIN32
        auto ret = LoadLibraryA(libraryFile.c_str());
#elif __APPLE__
        auto cfStringRef = createCFString(libraryFile.string().c_str());
        auto ret = CFBundleCreate(kCFAllocatorDefault,
            CFURLCreateWithFileSystemPath(kCFAllocatorDefault,
                cfStringRef,
                kCFURLPOSIXPathStyle,
                false));
#else
        auto ret = dlopen(libraryFile.c_str(), RTLD_LAZY | RTLD_LOCAL);
#endif
        //if (errno)
        //    std::cerr << "dlopen resulted in error: " << dlerror() << std::endl;
        //assert(ret);
        return ret;
    }

    int32_t initializeModule(void* library) {
#if _WIN32
        auto module = (HMODULE) library;
        auto initDll = (init_dll_func) GetProcAddress(module, "initDll");
        if (initDll) // optional
            initDll();
#elif __APPLE__
        auto bundle = (CFBundleRef) library;
        if (!CFBundleLoadExecutable(bundle))
            return -1;
        auto bundleEntry = (vst3_bundle_entry_func) CFBundleGetFunctionPointerForName(bundle, createCFString("bundleEntry"));
        if (!bundleEntry)
            return -2;
        // check this in prior (not calling now).
        auto bundleExit = !bundleEntry ? nullptr : CFBundleGetFunctionPointerForName(bundle, createCFString("bundleExit"));
        if (!bundleExit)
            return -3;
        if (!bundleEntry(bundle))
            return -4;
#else
        auto moduleEntry = (vst3_module_entry_func) dlsym(library, "ModuleEntry");
        if (!errno)
            dlsym(library, "ModuleExit"); // check this in prior.
        if (errno)
            return errno;
        moduleEntry(library);
#endif
        return 0;
    }

    void unloadLibrary(void* library) {
#if _WIN32
        auto exitDll = (vst3_exit_dll_func) GetProcAddress(module, "exitDll");
        if (exitDll) // optional
            exitDll();
        FreeLibrary((HMODULE) library);
#elif __APPLE__
        auto bundle = (CFBundleRef) library;
        auto bundleExit = (vst3_bundle_exit_func) CFBundleGetFunctionPointerForName(bundle, createCFString("bundleExit"));
        if (bundleExit) // it might not exist, as it may fail to load the library e.g. ABI mismatch.
            bundleExit();
        CFBundleUnloadExecutable(bundle);
#else
        auto moduleExit = (vst3_module_exit_func) dlsym(library, "ModuleExit");
        moduleExit(); // no need to check existence, it's done at loading.
        dlclose(library);
#endif
    }

    IPluginFactory* getFactoryFromLibrary(void* library) {
#if _WIN32
        auto sym = (get_plugin_factory_func) GetProcAddress((HMODULE) library, "GetPluginFactory");
#elif __APPLE__
        auto bundle = (CFBundleRef) library;
        auto sym = (get_plugin_factory_func) CFBundleGetFunctionPointerForName(bundle, createCFString("GetPluginFactory"));
#else
        auto sym = (get_plugin_factory_func) dlsym(library, "GetPluginFactory");
#endif
        assert(sym);
        return sym();
    }

    void AudioPluginFormatVST3::Impl::forEachPlugin(std::filesystem::path vst3Dir,
        std::function<void(void* module, IPluginFactory* factory, PluginClassInfo& info)> func,
        std::function<void(void* module)> cleanup
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
                    std::cerr << ": failed to retrieve class info at " << i << ", in " << vst3Dir.string() << std::endl;
            }
            cleanup(module);
        }
        else
            std::cerr << "Could not load the library from bundle: " << vst3Dir.c_str() << std::endl;

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
