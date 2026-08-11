#include <uapmd-engine/uapmd-engine.hpp>

#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>
#include <utility>

#if defined(UAPMD_HAS_CPPLOCATE)
    #include <cpplocate/cpplocate.h>
#endif

#if defined(_WIN32)
    #define NOMINMAX
    #include <windows.h>
#elif !defined(__EMSCRIPTEN__)
    #include <dlfcn.h>
#endif

namespace uapmd {
namespace {

class EngineAddinHost final : public AddinHost {
public:
    void* extensionPoint(std::string_view) noexcept override {
        // Concrete extension points register their host interfaces here.
        return nullptr;
    }
};

std::string addinKey(std::string_view packageId, std::string_view addinId) {
    return std::string(packageId) + '\t' + std::string(addinId);
}

std::filesystem::path settingsPath() {
#if defined(UAPMD_HAS_CPPLOCATE)
    auto directory = cpplocate::localDir("uapmd");
    if (directory.empty())
        return {};
    std::error_code error;
    const std::filesystem::path path(directory);
    std::filesystem::create_directories(path, error);
    if (error)
        return {};
    return path / "addins.conf";
#else
    return {};
#endif
}

std::filesystem::path addinDirectoryPath() {
#if defined(UAPMD_HAS_CPPLOCATE)
    auto directory = cpplocate::localDir("uapmd");
    if (directory.empty())
        return {};
    std::error_code error;
    const std::filesystem::path path = std::filesystem::path(directory) / "addins";
    std::filesystem::create_directories(path, error);
    return error ? std::filesystem::path{} : path;
#else
    return {};
#endif
}

bool isDynamicLibraryPath(const std::filesystem::path& path) {
#if defined(_WIN32)
    return path.extension() == ".dll";
#elif defined(__APPLE__)
    return path.extension() == ".dylib";
#else
    return path.extension() == ".so";
#endif
}

} // namespace

class AddinManager::Impl {
public:
    struct LoadedAddin {
        uapmd::Addin* addin = nullptr;
        AddinInfo info;
    };

    struct Library {
        std::filesystem::path path;
        void* handle = nullptr;
        uapmd::AddinEntry* entry = nullptr;
        std::vector<LoadedAddin> addins;
    };

    EngineAddinHost host;
    std::vector<std::unique_ptr<Library>> libraries;
    std::vector<AddinInfo> addin_infos;
    std::map<std::string, bool, std::less<>> enabled_settings;
    std::filesystem::path addin_directory = addinDirectoryPath();
    std::string last_error;

    Impl() {
        loadSettings();
    }

    ~Impl() {
        shutdown();
    }

    void loadSettings() {
        const auto path = settingsPath();
        if (path.empty())
            return;
        std::ifstream input(path);
        std::string line;
        while (std::getline(input, line)) {
            if (!line.starts_with("addin\t"))
                continue;
            const auto packageEnd = line.find('\t', 6);
            const auto addinEnd = packageEnd == std::string::npos ? std::string::npos : line.find('\t', packageEnd + 1);
            if (packageEnd == std::string::npos || addinEnd == std::string::npos)
                continue;
            const auto packageId = line.substr(6, packageEnd - 6);
            const auto addinId = line.substr(packageEnd + 1, addinEnd - packageEnd - 1);
            enabled_settings[addinKey(packageId, addinId)] = line.substr(addinEnd + 1) == "1";
        }
    }

    void saveSettings() const {
        const auto path = settingsPath();
        if (path.empty())
            return;
        std::ofstream output(path, std::ios::trunc);
        for (const auto& [key, enabled] : enabled_settings) {
            const auto separator = key.find('\t');
            if (separator == std::string::npos)
                continue;
            output << "addin\t" << key.substr(0, separator) << '\t'
                   << key.substr(separator + 1) << '\t' << (enabled ? 1 : 0) << '\n';
        }
    }

    void rebuildInfoSnapshot() {
        addin_infos.clear();
        for (const auto& library : libraries)
            for (const auto& addin : library->addins)
                addin_infos.push_back(addin.info);
    }

    static void closeLibrary(void* handle) noexcept {
#if defined(_WIN32)
        if (handle)
            FreeLibrary(static_cast<HMODULE>(handle));
#elif !defined(__EMSCRIPTEN__)
        if (handle)
            dlclose(handle);
#else
        (void) handle;
#endif
    }

    static void* openLibrary(const std::filesystem::path& path, std::string& error) {
#if defined(_WIN32)
        auto handle = LoadLibraryW(path.c_str());
        if (!handle)
            error = "LoadLibraryW failed";
        return handle;
#elif !defined(__EMSCRIPTEN__)
        dlerror();
        auto handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!handle) {
            const char* loaderError = dlerror();
            error = loaderError ? loaderError : "dlopen failed";
        }
        return handle;
#else
        (void) path;
        error = "Dynamic addin loading is unavailable in WebAssembly builds";
        return nullptr;
#endif
    }

    static void* findSymbol(void* handle, std::string& error) {
#if defined(_WIN32)
        auto symbol = GetProcAddress(static_cast<HMODULE>(handle), "uapmd_addin_entry");
        if (!symbol)
            error = "uapmd_addin_entry is not exported";
        return reinterpret_cast<void*>(symbol);
#elif !defined(__EMSCRIPTEN__)
        dlerror();
        auto symbol = dlsym(handle, "uapmd_addin_entry");
        if (const char* loaderError = dlerror(); loaderError)
            error = loaderError;
        return symbol;
#else
        (void) handle;
        error = "Dynamic addin loading is unavailable in WebAssembly builds";
        return nullptr;
#endif
    }

    Library* findLibrary(const std::filesystem::path& path) {
        const auto normalized = std::filesystem::absolute(path).lexically_normal();
        const auto it = std::find_if(libraries.begin(), libraries.end(), [&](const auto& library) {
            return library->path == normalized;
        });
        return it == libraries.end() ? nullptr : it->get();
    }

    LoadedAddin* findAddin(std::string_view packageId, std::string_view addinId) {
        for (const auto& library : libraries)
            for (auto& addin : library->addins)
                if (addin.info.package_id == packageId && addin.info.addin_id == addinId)
                    return &addin;
        return nullptr;
    }

    bool activate(LoadedAddin& loaded) {
        if (loaded.info.state == AddinState::Active)
            return true;
        loaded.info.state = AddinState::Initializing;
        loaded.info.message.clear();
        if (!loaded.addin->initialize(host)) {
            loaded.info.state = AddinState::Failed;
            loaded.info.message = "initialize() returned false";
            return false;
        }
        loaded.info.state = AddinState::Active;
        return true;
    }

    bool deactivate(LoadedAddin& loaded) {
        if (loaded.info.state != AddinState::Active)
            return loaded.info.state != AddinState::Initializing && loaded.info.state != AddinState::CleaningUp;
        loaded.info.state = AddinState::CleaningUp;
        loaded.addin->cleanup(host);
        loaded.info.state = AddinState::Inactive;
        loaded.info.message.clear();
        return true;
    }

    bool loadLibrary(const std::filesystem::path& path, bool ignoreMissingEntry) {
        const auto normalized = std::filesystem::absolute(path).lexically_normal();
        if (findLibrary(normalized))
            return true;

        std::string loaderError;
        void* handle = openLibrary(normalized, loaderError);
        if (!handle) {
            last_error = "Failed to load " + normalized.string() + ": " + loaderError;
            return false;
        }
        void* symbol = findSymbol(handle, loaderError);
        if (!symbol) {
            closeLibrary(handle);
            if (ignoreMissingEntry)
                return false;
            last_error = "Failed to resolve addin entry: " + loaderError;
            return false;
        }

        auto entryFunction = reinterpret_cast<AddinEntryFunction>(symbol);
        auto* entry = entryFunction();
        if (!entry || entry->packageId().empty()) {
            closeLibrary(handle);
            last_error = "Addin entry is null or has no package ID";
            return false;
        }

        auto library = std::make_unique<Library>();
        library->path = normalized;
        library->handle = handle;
        library->entry = entry;
        for (Addin* addin : entry->addins()) {
            if (!addin) {
                closeLibrary(handle);
                last_error = "Addin entry contains a null addin";
                return false;
            }
            const auto identity = addin->identity();
            if (identity.package_id.empty() || identity.addin_id.empty() || addin->name().empty() || addin->path().empty()) {
                closeLibrary(handle);
                last_error = "Addin metadata must include package ID, addin ID, name, and path";
                return false;
            }
            if (identity.package_id != entry->packageId()) {
                closeLibrary(handle);
                last_error = "Addin package ID does not match its entry";
                return false;
            }
            const bool duplicate = std::ranges::any_of(library->addins, [&](const auto& existing) {
                return existing.info.addin_id == identity.addin_id;
            });
            if (duplicate) {
                closeLibrary(handle);
                last_error = "Addin package contains duplicate addin IDs";
                return false;
            }
            library->addins.push_back({
                .addin = addin,
                .info = {
                    .package_id = std::string(identity.package_id),
                    .addin_id = std::string(identity.addin_id),
                    .name = std::string(addin->name()),
                    .path = std::string(addin->path()),
                    .library_path = normalized,
                },
            });
        }
        if (library->addins.empty()) {
            closeLibrary(handle);
            last_error = "Addin package contains no addins";
            return false;
        }

        for (auto& addin : library->addins) {
            const auto setting = enabled_settings.find(addinKey(addin.info.package_id, addin.info.addin_id));
            const bool enabled = setting == enabled_settings.end() || setting->second;
            if (enabled) {
                if (!activate(addin))
                    last_error = addin.info.name + ": " + addin.info.message;
            }
        }
        libraries.push_back(std::move(library));
        rebuildInfoSnapshot();
        return true;
    }

    void shutdown() {
        for (auto library = libraries.rbegin(); library != libraries.rend(); ++library) {
            for (auto addin = (*library)->addins.rbegin(); addin != (*library)->addins.rend(); ++addin)
                deactivate(*addin);
            closeLibrary((*library)->handle);
            (*library)->handle = nullptr;
            (*library)->entry = nullptr;
        }
        libraries.clear();
        rebuildInfoSnapshot();
    }
};

AddinManager::AddinManager()
    : impl_(std::make_unique<Impl>()) {
    loadInstalledAddins();
}

AddinManager::~AddinManager() = default;

bool AddinManager::supportsDynamicLoading() noexcept {
#if defined(__EMSCRIPTEN__)
    return false;
#else
    return true;
#endif
}

void AddinManager::loadInstalledAddins() {
    impl_->last_error.clear();
    if (!supportsDynamicLoading()) {
        return;
    }
    if (impl_->addin_directory.empty()) {
        impl_->last_error = "No addin directory is available on this platform";
        return;
    }
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(impl_->addin_directory, error)) {
        if (error) {
            impl_->last_error = "Failed to scan addin directory: " + error.message();
            return;
        }
        if (!entry.is_regular_file(error) || error || !isDynamicLibraryPath(entry.path()))
            continue;
        impl_->loadLibrary(entry.path(), true);
    }
}

bool AddinManager::setEnabled(const std::string& packageId, const std::string& addinId, bool enabled) {
    impl_->last_error.clear();
    auto* addin = impl_->findAddin(packageId, addinId);
    if (!addin) {
        impl_->last_error = "Addin is no longer loaded";
        return false;
    }
    const bool success = enabled ? impl_->activate(*addin) : impl_->deactivate(*addin);
    impl_->enabled_settings[addinKey(packageId, addinId)] = enabled && success;
    impl_->saveSettings();
    impl_->rebuildInfoSnapshot();
    if (!success)
        impl_->last_error = addin->info.name + ": " + addin->info.message;
    return success;
}

void AddinManager::shutdown() {
    impl_->shutdown();
}

const std::vector<AddinInfo>& AddinManager::addins() const noexcept {
    return impl_->addin_infos;
}

const std::string& AddinManager::lastError() const noexcept {
    return impl_->last_error;
}

const std::filesystem::path& AddinManager::addinDirectory() const noexcept {
    return impl_->addin_directory;
}

const char* addinStateName(AddinState state) noexcept {
    switch (state) {
        case AddinState::Inactive: return "Inactive";
        case AddinState::Initializing: return "Initializing";
        case AddinState::Active: return "Active";
        case AddinState::CleaningUp: return "Cleaning up";
        case AddinState::Failed: return "Failed";
    }
    return "Unknown";
}

} // namespace uapmd
