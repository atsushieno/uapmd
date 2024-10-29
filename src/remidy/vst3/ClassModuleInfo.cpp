
#include <vector>
#include <fstream>
#include <sstream>
#include "ClassModuleInfo.hpp"

#include <priv/common.hpp>

#include "moduleinfoparser.h"

#include "../utils.hpp"

#if __APPLE__
#include <CoreFoundation/CoreFoundation.h>
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
    // It might fail due to ABI mismatch on macOS. We have to ignore the error and return nullptr.
    void* loadLibraryFromBinary(std::filesystem::path& vst3Dir) {
#if _WIN32
        auto ret = LoadLibraryA(libraryFile.c_str());
#elif __APPLE__
        auto cfStringRef = createCFString(vst3Dir.string().c_str());
        auto ret = CFBundleCreate(kCFAllocatorDefault,
            CFURLCreateWithFileSystemPath(kCFAllocatorDefault,
                cfStringRef,
                kCFURLPOSIXPathStyle,
                false));
#else
        auto ret = dlopen(libraryFile.c_str(), RTLD_LAZY | RTLD_LOCAL);
#endif
        //if (errno)
        //    defaultLogError("dlopen resulted in error: %s", dlerror());
        return ret;
    }

    int32_t initializeModule(void* module) {
#if _WIN32
        auto module = (HMODULE) module;
        auto initDll = (init_dll_func) GetProcAddress(module, "initDll");
        if (initDll) // optional
            initDll();
#elif __APPLE__
        auto bundle = (CFBundleRef) module;
        if (!CFBundleLoadExecutable(bundle))
            return -1;
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
        if (!errno)
            dlsym(module, "ModuleExit"); // check this in prior.
        if (errno)
            return errno;
        moduleEntry(module);
#endif
        return 0;
    }

    void unloadModule(void* moduleBundle) {
#if _WIN32
        auto exitDll = (vst3_exit_dll_func) GetProcAddress(module, "exitDll");
        if (exitDll) // optional
            exitDll();
        FreeLibrary((HMODULE) moduleBundle);
#elif __APPLE__
        auto bundle = (CFBundleRef) moduleBundle;
        auto bundleExit = (vst3_bundle_exit_func) CFBundleGetFunctionPointerForName(bundle, createCFString("bundleExit"));
        if (bundleExit) // it might not exist, as it may fail to load the library e.g. ABI mismatch.
            bundleExit();
        CFRelease(bundle);
#else
        auto moduleExit = (vst3_module_exit_func) dlsym(library, "ModuleExit");
        moduleExit(); // no need to check existence, it's done at loading.
        dlclose(moduleBundle);
#endif
    }
}
