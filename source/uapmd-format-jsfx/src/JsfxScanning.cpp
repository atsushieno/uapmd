#include "JsfxScanning.hpp"

#include <algorithm>
#include <cstdlib>
#include <system_error>

#include "ysfx.h"

#include "uapmd-format-jsfx/uapmd-format-jsfx.hpp"

namespace uapmd_jsfx {

    namespace {
        // JSFX effects are conventionally extensionless -- 219 of the 225 files in a stock
        // REAPER effects folder have no extension at all -- so candidates cannot be chosen
        // by extension. They are ruled out by it instead.
        //
        // `.jsfx-inc` matters most here: include files parse as JSFX perfectly well and
        // would otherwise be offered as plugins that cannot run. `.rpl` preset banks sit
        // beside the effects they belong to and are read later, by name, not by scanning.
        bool hasDeniedExtension(const std::filesystem::path& path) {
            auto ext = path.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (ext.empty())
                return false;
            static const char* denied[] = {
                ".rpl", ".bak", ".jsfx-inc", ".inc",
                ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".svg", ".ico",
                ".wav", ".ogg", ".flac", ".mp3", ".aif", ".aiff", ".raw",
                ".txt", ".md", ".pdf", ".rtf", ".html", ".htm", ".xml", ".json", ".ini", ".cfg",
                ".zip", ".gz", ".tar", ".7z", ".rar",
                ".dll", ".so", ".dylib", ".exe", ".a", ".lib", ".o",
            };
            for (const char* d : denied)
                if (ext == d)
                    return true;
            return false;
        }

        // Effects are text. The largest in a stock install is about 120 KB, so anything far
        // beyond that is something else that happens to live in the tree.
        constexpr std::uintmax_t kMaxCandidateBytes = 4u * 1024u * 1024u;

        std::string environmentPath(const char* name) {
            const char* value = std::getenv(name);
            return value ? std::string{value} : std::string{};
        }

        // Where this host keeps effects of its own: what the user imported, and the
        // mirrors of the folders they registered. Always searched, and not a setting --
        // turning them off would hide the user's own effects from them, and they are
        // wherever this installation happens to keep its files rather than somewhere
        // anyone chose. The two are kept apart so that rebuilding a mirror cannot take
        // an import with it, and so that plugin ids read as "<folder>/<effect>".
        std::vector<std::filesystem::path> hostOwnedSearchPaths() {
            std::vector<std::filesystem::path> paths{};
            if (auto userContent = jsfxUserContentDirectory(); !userContent.empty())
                paths.emplace_back(std::move(userContent));
            if (auto mirrors = jsfxFolderMirrorRoot(); !mirrors.empty())
                paths.emplace_back(std::move(mirrors));
            return paths;
        }

        // Where this platform conventionally keeps JSFX effects: REAPER's own folder.
        // This is the part the user may decline to search.
        std::vector<std::filesystem::path> platformDefaultSearchPaths() {
            std::vector<std::filesystem::path> paths{};

#if defined(__EMSCRIPTEN__)
            // There is no host filesystem; everything arrives through the browser, and the
            // user content directory above is IDBFS-backed so imports survive a reload.
#elif defined(_WIN32)
            auto appData = environmentPath("APPDATA");
            if (!appData.empty())
                paths.emplace_back(std::filesystem::path{appData} / "REAPER" / "Effects");
#elif defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
            // No REAPER installation to borrow from; the user content directory above is
            // the whole story unless the user adds search paths of their own.
#elif defined(__APPLE__)
            auto home = environmentPath("HOME");
            if (!home.empty())
                paths.emplace_back(std::filesystem::path{home} /
                                   "Library" / "Application Support" / "REAPER" / "Effects");
#else
            auto configHome = environmentPath("XDG_CONFIG_HOME");
            if (!configHome.empty())
                paths.emplace_back(std::filesystem::path{configHome} / "REAPER" / "Effects");
            else {
                auto home = environmentPath("HOME");
                if (!home.empty())
                    paths.emplace_back(std::filesystem::path{home} / ".config" / "REAPER" / "Effects");
            }
#endif
            return paths;
        }

        // Plugin ids go into saved projects, so they are spelled the same way on every
        // platform. REAPER names its effects this way too.
        std::string toPluginId(const std::filesystem::path& root, const std::filesystem::path& file) {
            std::error_code ec{};
            auto relative = std::filesystem::relative(file, root, ec);
            const auto& chosen = ec ? file : relative;
            return chosen.generic_string();
        }
    }

    JsfxScanning::JsfxScanning() {
        refreshDefaultSearchPaths();
    }

    void JsfxScanning::refreshDefaultSearchPaths() const {
        default_search_paths_ = platformDefaultSearchPaths();
    }

    std::vector<std::filesystem::path> JsfxScanning::activeSearchRoots() const {
        std::vector<std::filesystem::path> roots{};
        // Searched whatever the settings say: these hold the effects the user gave this
        // host to keep, and there would be no way to get at them again otherwise.
        for (auto& path : hostOwnedSearchPaths())
            roots.emplace_back(std::move(path));
        refreshDefaultSearchPaths();
        if (useDefaultSearchPaths())
            for (auto& path : default_search_paths_)
                roots.emplace_back(path);
        // getOverrideSearchPaths() is not const in the base, but reading it here is.
        auto& overrides = const_cast<JsfxScanning*>(this)->getOverrideSearchPaths();
        for (auto& path : overrides)
            roots.emplace_back(path);
        return roots;
    }

    std::optional<uapmd_plugin_hosting::AudioPluginCatalogEntry> describeJsfxFile(
            const std::filesystem::path& path, const std::string& pluginId) {
        ysfx_config_u config{ysfx_config_new()};
        if (!config)
            return {};
        // Scanning must stay silent. A folder full of files that are not effects would
        // otherwise fill the log with parse errors that are not the user's problem.
        ysfx_set_log_reporter(config.get(), [](intptr_t, ysfx_log_level, const char*) {});

        ysfx_u fx{ysfx_new(config.get())};
        if (!fx)
            return {};

        // Imports are not resolved: the header is all that is needed, and resolving them
        // would drag in files that may not exist on this machine.
        if (!ysfx_load_file(fx.get(), path.string().c_str(), ysfx_load_ignoring_imports))
            return {};

        const char* name = ysfx_get_name(fx.get());
        const bool named = name && *name;
        const bool hasCode =
                ysfx_has_section(fx.get(), ysfx_section_init) ||
                ysfx_has_section(fx.get(), ysfx_section_slider) ||
                ysfx_has_section(fx.get(), ysfx_section_block) ||
                ysfx_has_section(fx.get(), ysfx_section_sample) ||
                ysfx_has_section(fx.get(), ysfx_section_gfx);
        if (!named && !hasCode)
            return {};

        uapmd_plugin_hosting::AudioPluginCatalogEntry entry{};
        entry.format(kFormatName);
        entry.pluginId(pluginId);
        // Some older effects carry no `desc:` line; the file name is what REAPER shows for
        // those, so it is what we show too.
        entry.displayName(named ? name : path.filename().string());
        const char* author = ysfx_get_author(fx.get());
        entry.vendorName(author ? author : "");
        entry.bundlePath(path);
        return entry;
    }

    std::vector<std::filesystem::path> JsfxScanning::enumerateCandidateBundles(bool requireFastScanning) {
        (void) requireFastScanning;  // every JSFX file is fast to scan.
        std::vector<std::filesystem::path> candidates{};
        for (auto& root : activeSearchRoots()) {
            std::error_code ec{};
            if (!std::filesystem::is_directory(root, ec))
                continue;
            auto options = std::filesystem::directory_options::skip_permission_denied;
            for (std::filesystem::recursive_directory_iterator it{root, options, ec}, end{};
                 it != end; it.increment(ec)) {
                if (ec) {
                    ec.clear();
                    continue;
                }
                auto name = it->path().filename().string();
                // Version control and package manager bookkeeping is not content.
                if (!name.empty() && name.front() == '.') {
                    if (it->is_directory(ec))
                        it.disable_recursion_pending();
                    continue;
                }
                if (!it->is_regular_file(ec))
                    continue;
                if (hasDeniedExtension(it->path()))
                    continue;
                auto size = it->file_size(ec);
                if (ec) {
                    ec.clear();
                    continue;
                }
                if (size == 0 || size > kMaxCandidateBytes)
                    continue;
                candidates.emplace_back(it->path());
            }
        }
        return candidates;
    }

    std::vector<uapmd_plugin_hosting::AudioPluginCatalogEntry> JsfxScanning::getAllFastScannablePlugins() {
        std::vector<uapmd_plugin_hosting::AudioPluginCatalogEntry> entries{};
        std::vector<std::string> seen{};

        for (auto& root : activeSearchRoots()) {
            std::error_code ec{};
            if (!std::filesystem::is_directory(root, ec))
                continue;
            auto options = std::filesystem::directory_options::skip_permission_denied;
            for (std::filesystem::recursive_directory_iterator it{root, options, ec}, end{};
                 it != end; it.increment(ec)) {
                if (ec) {
                    ec.clear();
                    continue;
                }
                auto name = it->path().filename().string();
                if (!name.empty() && name.front() == '.') {
                    if (it->is_directory(ec))
                        it.disable_recursion_pending();
                    continue;
                }
                if (!it->is_regular_file(ec) || hasDeniedExtension(it->path()))
                    continue;
                auto size = it->file_size(ec);
                if (ec) {
                    ec.clear();
                    continue;
                }
                if (size == 0 || size > kMaxCandidateBytes)
                    continue;

                auto pluginId = toPluginId(root, it->path());
                // Two roots can hold the same relative path. The first root that has it
                // wins, so that a plugin id keeps meaning the same effect for as long as
                // the search paths are unchanged.
                if (std::find(seen.begin(), seen.end(), pluginId) != seen.end())
                    continue;

                auto entry = describeJsfxFile(it->path(), pluginId);
                if (!entry)
                    continue;
                seen.emplace_back(pluginId);
                entries.emplace_back(std::move(*entry));
            }
        }
        return entries;
    }

    void JsfxScanning::startSlowPluginScan(uapmd_plugin_hosting::AudioPluginFoundCallback pluginFound,
                                           uapmd_plugin_hosting::AudioPluginScanCompletedCallback scanCompleted) {
        // Nothing about JSFX is slow to scan; the fast pass already found everything.
        (void) pluginFound;
        if (scanCompleted)
            scanCompleted("");
    }

    void JsfxScanning::scanBundle(const std::filesystem::path& bundlePath,
                                  bool requireFastScanning,
                                  double timeoutSeconds,
                                  uapmd_plugin_hosting::AudioPluginFoundCallback pluginFound,
                                  uapmd_plugin_hosting::AudioPluginScanCompletedCallback scanCompleted) {
        (void) requireFastScanning;
        (void) timeoutSeconds;  // reading one text file cannot hang.

        std::string pluginId = bundlePath.generic_string();
        for (auto& root : activeSearchRoots()) {
            std::error_code ec{};
            auto relative = std::filesystem::relative(bundlePath, root, ec);
            if (ec || relative.empty())
                continue;
            if (relative.generic_string().rfind("..", 0) == 0)
                continue;
            pluginId = relative.generic_string();
            break;
        }

        auto entry = describeJsfxFile(bundlePath, pluginId);
        if (entry && pluginFound)
            pluginFound(std::move(*entry));
        if (scanCompleted)
            scanCompleted("");
    }

    std::optional<std::filesystem::path> JsfxScanning::resolvePluginId(const std::string& pluginId) const {
        std::error_code ec{};
        for (auto& root : activeSearchRoots()) {
            auto candidate = root / std::filesystem::path{pluginId};
            if (std::filesystem::is_regular_file(candidate, ec))
                return candidate;
        }
        // A project may also have stored an absolute path, from a root that has since been
        // removed from the search paths.
        std::filesystem::path direct{pluginId};
        if (direct.is_absolute() && std::filesystem::is_regular_file(direct, ec))
            return direct;
        return {};
    }

}
