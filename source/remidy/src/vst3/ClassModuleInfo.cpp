
#include <vector>
#include <fstream>
#include <sstream>
#include <cstring>
#include <algorithm>
#include <cctype>
#include <string_view>
#include <type_traits>
#include "ClassModuleInfo.hpp"

#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include "moduleinfoparser.h"

#include "remidy/remidy.hpp"
#include "../utils.hpp"

#if WIN32
#include <Windows.h>
#elif __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#elif defined(__linux__)
#include <dlfcn.h>
#include <sys/utsname.h>
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
            std::string cid = stringToVst3Tuid(cls.cid);
            Steinberg::TUID tuid{};
            const auto bytesToCopy = std::min<size_t>(cid.size(), sizeof(Steinberg::TUID));
            memcpy(tuid, cid.data(), bytesToCopy);
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

    bool hasExtensionIgnoreCase(const std::filesystem::path& file, std::string_view lowerExt) {
        const auto extPath = file.extension();
        const auto& ext = extPath.native();
        if (ext.size() != lowerExt.size())
            return false;
        for (size_t i = 0; i < ext.size(); i++) {
            auto c = static_cast<std::make_unsigned_t<std::filesystem::path::value_type>>(ext[i]);
            if (c > 0x7F || std::tolower(static_cast<int>(c)) != lowerExt[i])
                return false;
        }
        return true;
    }

    // Sorted, as directory enumeration order is unspecified.
    std::vector<std::filesystem::path> findFilesWithExtension(const std::filesystem::path& dir, std::string_view lowerExt, bool recursive) {
        std::vector<std::filesystem::path> ret;
        auto collect = [&](const std::filesystem::directory_entry& entry) {
            std::error_code ec;
            if (entry.is_regular_file(ec) && hasExtensionIgnoreCase(entry.path(), lowerExt))
                ret.emplace_back(entry.path());
        };
        std::error_code ec;
        if (recursive) {
            for (std::filesystem::recursive_directory_iterator it{dir, std::filesystem::directory_options::skip_permission_denied, ec}, end; !ec && it != end; it.increment(ec))
                collect(*it);
        } else {
            for (std::filesystem::directory_iterator it{dir, ec}, end; !ec && it != end; it.increment(ec))
                collect(*it);
        }
        std::ranges::sort(ret);
        return ret;
    }

    void appendCandidate(std::vector<std::filesystem::path>& list, const std::filesystem::path& file) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(file, ec) && std::ranges::find(list, file) == list.end())
            list.emplace_back(file);
    }

    bool exportsPluginFactory(void* module) {
#if _WIN32
        return GetProcAddress((HMODULE) module, "GetPluginFactory") != nullptr;
#elif __APPLE__
        return getLibrarySymbol(module, "GetPluginFactory") != nullptr;
#else
        return dlsym(module, "GetPluginFactory") != nullptr;
#endif
    }

    void releaseLibrary(void* module) {
#if _WIN32
        FreeLibrary((HMODULE) module);
#elif __APPLE__
        unloadLibrary(module);
#else
        dlclose(module);
#endif
    }

    // Only a library that exports GetPluginFactory is accepted as a VST3 module.
    void* loadPluginLibrary(std::filesystem::path file) {
        auto module = loadLibraryFromBinary(file);
        if (!module)
            return nullptr;
        if (exportsPluginFactory(module))
            return module;
        releaseLibrary(module);
        return nullptr;
    }

#if !__APPLE__
    // Candidates in order of preference: the spec-compliant binary first, then lenient fallbacks.
    std::vector<std::filesystem::path> getPluginCodeFileCandidates(const std::filesystem::path& pluginPath) {
        std::error_code ec;
        if (!std::filesystem::is_directory(pluginPath, ec)) // self-contained plugin DLL
            return {pluginPath};
        // The ABI subdirectory name is VST3 specific. Therefore, this function is only usable with VST3.
        // But similar code structure would be usable with other plugin formats.

        // https://steinbergmedia.github.io/vst3_dev_portal/pages/Technical+Documentation/Locations+Format/Plugin+Format.html
        std::vector<std::filesystem::path> ret;
#if _WIN32
#if defined(__x86_64) || defined(__x86_64__) || defined(__amd64) || defined(_M_X64) || defined(__amd64__) || defined(_M_AMD64)
        auto abiDirName = "x86_64-win";
#elif defined(__i386) || defined(__i386__) || defined(_M_IX86) || defined(i386)  || defined(_M_IX86) || defined(_X86_) || defined(__THW_INTEL)
        auto abiDirName = "x86_32-win";
#elif defined(__ARM64_ARCH_8__) || defined(__aarch64__) || defined(__ARMv8__) || defined(__ARMv8_A__) || defined(_M_ARM64)
        // FIXME: there are also arm64-win and arm64x-win
        auto abiDirName = "arm64ec-win";
#else // at this state we assume the only remaining platform is arm32.
        auto abiDirName = "arm32-win";
#endif
        auto binDir = pluginPath / "Contents" / abiDirName;
        // The spec requires the binary to have the same name as the bundle.
        appendCandidate(ret, binDir / pluginPath.filename());
        // Renamed bundles, and then any .vst3 binary in the bundle, as JUCE-based hosts accept them.
        for (auto& file : findFilesWithExtension(binDir, ".vst3", false))
            appendCandidate(ret, file);
        for (auto& file : findFilesWithExtension(pluginPath, ".vst3", true))
            appendCandidate(ret, file);
#else
        // The ABI directory is named after the machine hardware name, as the SDK and JUCE do.
        utsname unameData{};
        if (uname(&unameData) != 0)
            return ret;
        auto binDir = pluginPath / "Contents" / (std::string{unameData.machine} + "-linux");
        // The spec requires the shared library to have the same name as the bundle.
        auto specFile = binDir / pluginPath.stem();
        specFile += ".so";
        appendCandidate(ret, specFile);
        // Renamed bundles.
        for (auto& file : findFilesWithExtension(binDir, ".so", false))
            appendCandidate(ret, file);
#endif
        return ret;
    }
#endif

    // may return nullptr if it failed to load.
    void* loadModuleFromVst3Path(std::filesystem::path vst3Dir) {
#if __APPLE__
        return loadPluginLibrary(vst3Dir);
#else
        for (auto& file : getPluginCodeFileCandidates(vst3Dir)) {
            if (auto module = loadPluginLibrary(file)) {
#if !_WIN32
                dlerror(); // clear errors left by rejected candidates, as initializeModule() checks dlerror().
#endif
                return module;
            }
        }
        return nullptr;
#endif
    }

    int32_t initializeModule(void* module) {
#if _WIN32
        auto initDll = (vst3_init_dll_func) GetProcAddress((HMODULE) module, "InitDll");
        if (initDll) // optional
            initDll();
#elif __APPLE__
        auto bundle = getLibraryBundle(module);
        if (!bundle)
            return -1;
        auto bundleEntry = (vst3_bundle_entry_func) getLibrarySymbol(module, "bundleEntry");
        if (!bundleEntry)
            return -2;
        // check this in prior (not calling now).
        auto bundleExit = getLibrarySymbol(module, "bundleExit");
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
        auto exitDll = (vst3_exit_dll_func) GetProcAddress(module, "ExitDll");
        if (exitDll) // optional
            exitDll();
        FreeLibrary(module);
#elif __APPLE__
        auto bundleExit = (vst3_bundle_exit_func) getLibrarySymbol(moduleBundle, "bundleExit");
        if (bundleExit) // it might not exist, as it may fail to load the library e.g. ABI mismatch.
            bundleExit();
        unloadLibrary(moduleBundle);
#else
        auto moduleExit = (vst3_module_exit_func) dlsym(moduleBundle, "ModuleExit");
        moduleExit(); // no need to check existence, it's done at loading.
        dlclose(moduleBundle);
#endif
    }
}
