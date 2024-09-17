#pragma once

#include <codecvt>
#include <filesystem>
#include <vector>

#include "../include/remidy/Common.hpp"
#include <travesty/factory.h>
#include <travesty/host.h>

#if __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#endif

#define kVstAudioEffectClass "Audio Module Class"

namespace remidy {
static constexpr const v3_tuid v3_component_iid =
    V3_ID(0xE831FF31, 0xF2D54301, 0x928EBBEE, 0x25697802);

    struct FUnknownVTable {
        v3_funknown unknown;
    };
    struct IPluginFactoryVTable : public FUnknownVTable {
        v3_plugin_factory factory;
    };
    struct IPluginFactory2VTable : public IPluginFactoryVTable {
        v3_plugin_factory_2 factory_2;
        v3_plugin_factory_3 factory_3;
    };
    struct IPluginFactory3VTable : public IPluginFactory2VTable {
        v3_plugin_factory_3 factory_3;
    };
    struct IPluginFactory {
        struct IPluginFactoryVTable *vtable;
    };
    struct IPluginFactory2 {
        struct IPluginFactoryVTable2 *vtable;
    };
    struct IPluginFactory3 {
        struct IPluginFactoryVTable3 *vtable;
    };
    struct IHostApplicationVTable : public FUnknownVTable {
        v3_host_application application;
    };
    struct IHostApplication {
        struct IHostApplicationVTable *vtable{};
    };
    class IHostApplicationDelegate : public IHostApplication {
        IHostApplicationVTable vt{};
        IPluginFactory* factory;
        static v3_result create_instance(void *self, v3_tuid cid, v3_tuid iid, void **obj);
        static v3_result get_name(void *self, v3_str_128 name);
    public:
        explicit IHostApplicationDelegate(IPluginFactory* factory) : factory(factory) {
            vtable = &vt;
            vt.unknown = factory->vtable->unknown;
            vt.application.create_instance = create_instance;
            vt.application.get_name = get_name;
        }
    };

    inline v3_result IHostApplicationDelegate::create_instance(void *self, v3_tuid cid, v3_tuid iid, void **obj) {
        auto h = (IHostApplicationDelegate*) self;
        return h->factory->vtable->factory.create_instance(h->factory, cid, iid, obj);
    }

    inline v3_result IHostApplicationDelegate::get_name(void *self, v3_str_128 name) {
        auto s = std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t>{}.from_bytes("remidy");
        s.copy((char16_t*) name, s.length());
    }

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

    typedef IPluginFactory* (*get_plugin_factory_func)();
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

    struct PluginClassInfo {
        std::filesystem::path bundlePath;
        std::string vendor;
        std::string url;
        std::string className;
        v3_tuid tuid{};

        PluginClassInfo(
            std::filesystem::path& bundlePath,
            std::string& vendor,
            std::string& url,
            std::string& className,
            v3_tuid tuid
        ): bundlePath(bundlePath), vendor(vendor), url(url), className(className) {
            memcpy(this->tuid, tuid, sizeof(tuid));
        }
    };

    void scanAllAvailablePluginsFromLibrary(std::filesystem::path vst3Dir, std::vector<PluginClassInfo>& results) {
        // FIXME: try to load moduleinfo.json and skip loading dynamic library.

        const auto library = loadLibraryFromBundle(vst3Dir);

        if (library) {
            auto err = initializeModule(library);
            if (err == 0) {
                auto factory = getFactoryFromLibrary(library);
                if (!factory)
                    return;

                v3_factory_info fInfo{};
                factory->vtable->factory.get_factory_info(factory, &fInfo);
                for (int i = 0, n = factory->vtable->factory.num_classes(factory); i < n; i++) {
                    v3_class_info cls{};
                    auto result = factory->vtable->factory.get_class_info(factory, i, &cls);
                    if (result == 0) {
                        //std::cerr << i << ": (" << cls.category << ") " << cls.name << std::endl;
                        // FIXME: can we check this in any better way?
                        if (!strcmp(cls.category, kVstAudioEffectClass)) {
                            std::string name = cls.name;
                            std::string vendor = fInfo.vendor;
                            std::string url = fInfo.url;
                            PluginClassInfo info(vst3Dir, vendor, url, name, cls.class_id);
                            results.emplace_back(info);
                        }
                    }
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
}