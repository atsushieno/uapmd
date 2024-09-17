
#include <cassert>
#include <dlfcn.h>
#include <iostream>
#include <travesty/factory.h>
#include "AudioPluginFormatVST3.hpp"

#if __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#endif

namespace remidy {
    struct IPluginFactoryVTable {
        v3_funknown unknown;
        v3_plugin_factory factory;
        v3_plugin_factory_2 factory_2;
        v3_plugin_factory_3 factory_3;
    };
    struct IPluginFactory {
        struct IPluginFactoryVTable *vtable;
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
    }

    bool AudioPluginFormatVST3::usePluginSearchPaths() { return true;}

    AudioPluginFormat::ScanningStrategyValue AudioPluginFormatVST3::scanRequiresLoadLibrary() { return YES; }

    AudioPluginFormat::ScanningStrategyValue AudioPluginFormatVST3::scanRequiresInstantiation() { return MAYBE; }

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

    typedef void* (*get_plugin_factory_func)();
    typedef bool (*vst3_module_entry_func)(void*);
    typedef bool (*vst3_module_exit_func)();
    typedef bool (*vst3_bundle_entry_func)(void*);
    typedef bool (*vst3_bundle_exit_func)();
    typedef bool (*vst3_init_dll_func)();
    typedef bool (*vst3_exit_dll_func)();

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

    CFStringRef createCFString(const char* s) {
        return CFStringCreateWithBytes(kCFAllocatorDefault, (UInt8*) s, strlen(s), CFStringEncoding{}, false);
    }

    // The returned library (platform dependent) must be released later (in the platform manner)
    // It might fail due to ABI mismatch on macOS. We have to ignore the error and return NULL.
    void* loadLibraryFromBinary(std::filesystem::path libraryFile) {
        std::cerr << libraryFile.string() << std::endl;
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

    void* getFactoryFromLibrary(void* library) {
#if _WIN32
        auto sym = (get_plugin_factory_func) GetProcAddress((HMODULE) library, "GetPluginFactory");
#elif __APPLE__
        auto bundle = (CFBundleRef) library;
        auto sym = (get_plugin_factory_func) CFBundleGetFunctionPointerForName(bundle, createCFString("GetPluginFactory"));
#else
        auto sym = (get_plugin_factory_func) dlsym(library, "GetPluginFactory");
        sym = sym ? sym : (get_plugin_factory_func) dlsym(library, "_GetPluginFactory");
#endif
        assert(sym);
        return sym();
    }

    void scanAllAvailablePluginsFromLibrary(std::filesystem::path vst3Dir, std::vector<AudioPluginIdentifier*> results) {
        // FIXME: try to load moduleinfo.json and skip loading dynamic library.

        const auto library = loadLibraryFromBundle(vst3Dir);

        if (library) {
            auto err = initializeModule(library);
            if (err == 0) {
                auto factory = (IPluginFactory*) getFactoryFromLibrary(library);
                if (!factory)
                    return;

                v3_factory_info fInfo{};
                factory->vtable->factory.get_factory_info(factory, &fInfo);
                std::cerr << "Vendor: " << fInfo.vendor << " / URL: " << fInfo.url << std::endl;
                for (int i = 0, n = factory->vtable->factory.num_classes(factory); i < n; i++) {
                    v3_class_info cls{};
                    auto result = factory->vtable->factory.get_class_info(factory, i, &cls);
                    if (result == 0)
                        std::cerr << i << ": (" << cls.category << ") " << cls.name << std::endl;
                    else
                        std::cerr << i << ": ERROR in " << cls.name << std::endl;
                }
            }
            else
                std::cerr << "Could not initialize the module from bundle: " << vst3Dir.c_str() << std::endl;
            unloadLibrary(library);
        }
        else
            std::cerr << "Could not load the library from bundle: " << vst3Dir.c_str() << " : " << dlerror() << std::endl;
    }

    std::vector<AudioPluginIdentifier*> AudioPluginFormatVST3::scanAllAvailablePlugins() {
        std::vector<AudioPluginIdentifier *> results;
        for (auto &path : getDefaultSearchPaths()) {
            std::filesystem::path dir{path};
            if (is_directory(dir)) {
                for (auto& entry : std::filesystem::directory_iterator(dir)) {
                    // FIXME: we need case-insensitive comparison
                    if (entry.path().extension() == ".vst3") {
                        scanAllAvailablePluginsFromLibrary(entry.path(), results);
                    }
                }
            }
        }
        return results;
    }

    AudioPluginInstance * AudioPluginFormatVST3::createInstance(AudioPluginIdentifier *uniqueId) {
        throw std::runtime_error("AudioPluginFormatVST3::createInstance() not implemented");
    }
}
