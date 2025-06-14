
#include <vector>
#include <fstream>
#include <sstream>
#include "ClassModuleInfo.hpp"

#include <priv/common.hpp>

#include "moduleinfoparser.h"

#include "../utils.hpp"

#if WIN32
#include <Windows.h>
#elif __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#elif defined(__linux__)
#include <dlfcn.h>
#endif

namespace remidy_vst3 {
    std::filesystem::path getModuleInfoFile(std::filesystem::path& bundlePath) {
        // obsolete file path
        std::filesystem::path p2{bundlePath};
        p2.append("Contents").append("moduleinfo.json");
        if (exists(p2))
            return p2;
        // standard file path
        std::filesystem::path p{bundlePath};
        p.append("Contents").append("Resources").append("moduleinfo.json");
        return p;
    }
    bool hasModuleInfo(std::filesystem::path& bundlePath) {
        return exists(getModuleInfoFile(bundlePath));
    }

    std::vector<PluginClassInfo> getModuleInfo(std::filesystem::path& bundlePath) {
        std::vector<PluginClassInfo> list;
        auto info = getModuleInfoFile(bundlePath);
        if (!std::filesystem::exists(info))
            return list;
        std::ifstream ifs{info.string()};
        std::ostringstream ofs;
        ofs << ifs.rdbuf();
        ifs.close();
        std::string str = ofs.str();

        std::ostringstream errorStream{};
        auto moduleInfo = Steinberg::ModuleInfoLib::parseJson(str, &errorStream);
        if (!moduleInfo.has_value()) {
            remidy::Logger::global()->logWarning("Failed to parse moduleinfo.json in %s : %s", bundlePath.c_str(), errorStream.str().c_str());
            return list; // failed to parse
        }
        auto factoryInfo = moduleInfo.value().factoryInfo;
        for (auto& cls : moduleInfo.value().classes) {
            if (strcmp(cls.category.c_str(), kVstAudioEffectClass))
                continue;
            std::string cid = stringToHexBinary(cls.cid);
            v3_tuid tuid{};
            memcpy(tuid, cid.c_str(), cid.length());
            std::string name = cls.name;
            std::string vendor = cls.vendor;
            auto entry = PluginClassInfo{bundlePath, cls.vendor, factoryInfo.url, cls.name, tuid};
            list.emplace_back(std::move(entry));
        };
        return list;
    }

    // bundle/library searcher and loader

    typedef bool (*vst3_module_entry_func)(void*);
    typedef bool (*vst3_module_exit_func)();
    typedef bool (*vst3_bundle_entry_func)(void*);
    typedef bool (*vst3_bundle_exit_func)();
    typedef bool (*vst3_init_dll_func)();
    typedef bool (*vst3_exit_dll_func)();

    std::filesystem::path getPluginCodeFile(std::filesystem::path& pluginPath) {
        // The ABI subdirectory name is VST3 specific. Therefore, this function is only usable with VST3.
        // But similar code structure would be usable with other plugin formats.

        // https://steinbergmedia.github.io/vst3_dev_portal/pages/Technical+Documentation/Locations+Format/Plugin+Format.html
#if _WIN32
#if __x86_64__
        auto abiDirName = "x86_64-win";
#elif __x86_32__
        auto abiDirName = "x86_32-win";
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

    // may return nullptr if it failed to load.
    void* loadModuleFromVst3Path(std::filesystem::path vst3Dir) {
#if __APPLE__
        const auto allBundles = CFBundleGetAllBundles();
        CFBundleRef bundle{nullptr};
        for (size_t i = 0, n = CFArrayGetCount(allBundles); i < n; i++) {
            bundle = (CFBundleRef) CFArrayGetValueAtIndex(allBundles, i);
            const auto url = CFBundleCopyBundleURL(bundle);
            const auto pathString = CFURLCopyPath(url);
            if (!strcmp(CFStringGetCStringPtr(pathString, kCFStringEncodingUTF8), vst3Dir.c_str())) {
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
            (const UInt8*) vst3Dir.c_str(),
            vst3Dir.string().size(),
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
        auto libraryFilePath = getPluginCodeFile(vst3Dir);
        if (!libraryFilePath.empty()) {
            return loadLibraryFromBinary(libraryFilePath);
        }
        else
            return nullptr;
#endif
    }

    int32_t initializeModule(void* module) {
#if _WIN32
        auto initDll = (vst3_init_dll_func) GetProcAddress((HMODULE) module, "initDll");
        if (initDll) // optional
            initDll();
#elif __APPLE__
        auto bundle = (CFBundleRef) module;
        CFErrorRef cfError;
        if (!CFBundleLoadExecutableAndReturnError(bundle, &cfError)) {
            // FIXME: we need logger here too
            std::cerr << "CFBundleLoadExecutableAndReturnError : " << cfError << std::endl;
            return -1;
        }
        auto bundleEntry = (vst3_bundle_entry_func) CFBundleGetFunctionPointerForName(bundle, createCFString("bundleEntry"));
        if (!bundleEntry)
            return -2;
        // check this in prior (not calling now).
        auto bundleExit = !module ? nullptr : CFBundleGetFunctionPointerForName(bundle, createCFString("bundleExit"));
        if (!bundleExit)
            return -3;
        if (!bundleEntry(bundle))
            return -4;
#else
        auto moduleEntry = (vst3_module_entry_func) dlsym(module, "ModuleEntry");
        auto err = dlerror();
        if (!err) {
            dlsym(module, "ModuleExit"); // check this in prior.
            err = dlerror();
        }
        if (err)
            return errno;
        moduleEntry(module);
#endif
        return 0;
    }

    void unloadModule(void* moduleBundle) {
#if _WIN32
        auto module = (HMODULE) moduleBundle;
        auto exitDll = (vst3_exit_dll_func) GetProcAddress(module, "exitDll");
        if (exitDll) // optional
            exitDll();
        FreeLibrary(module);
#elif __APPLE__
        auto bundle = (CFBundleRef) moduleBundle;
        auto bundleExit = (vst3_bundle_exit_func) CFBundleGetFunctionPointerForName(bundle, createCFString("bundleExit"));
        if (bundleExit) // it might not exist, as it may fail to load the library e.g. ABI mismatch.
            bundleExit();
        CFRelease(bundle);
#else
        auto moduleExit = (vst3_module_exit_func) dlsym(moduleBundle, "ModuleExit");
        moduleExit(); // no need to check existence, it's done at loading.
        dlclose(moduleBundle);
#endif
    }
}
