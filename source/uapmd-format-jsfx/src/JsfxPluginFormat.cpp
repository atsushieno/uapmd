#include "uapmd-format-jsfx/uapmd-format-jsfx.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <system_error>
#include <unordered_map>

#include "choc/text/choc_JSON.h"
#include "ysfx.h"

#if UAPMD_JSFX_OWN_FONTS
#include "JsfxFontBackend.hpp"
#endif
#include "JsfxPluginInstance.hpp"
#include "JsfxScanning.hpp"

namespace uapmd_jsfx {

    namespace {
        constexpr const char* kSettingsFileName = "jsfx-settings.json";

        std::filesystem::path settingsFile() {
            const auto& root = uapmd_plugin_hosting::applicationDataDirectory();
            return root.empty() ? std::filesystem::path{} : root / kSettingsFileName;
        }

        JsfxDuplicateListing loadDuplicateListing() {
            const auto file = settingsFile();
            std::error_code ec{};
            if (file.empty() || !std::filesystem::is_regular_file(file, ec))
                return JsfxDuplicateListing::PreferLatestVersion;
            try {
                std::ifstream stream{file};
                std::string text{std::istreambuf_iterator<char>(stream),
                                 std::istreambuf_iterator<char>()};
                auto root = choc::json::parse(text).getView();
                if (root.isObject() && root.hasObjectMember("duplicateListing") &&
                    root["duplicateListing"].isString()) {
                    const auto value = root["duplicateListing"].getString();
                    if (value == "showAll")
                        return JsfxDuplicateListing::ShowAll;
                    // Version 1 offered only exact-copy hiding. Migrate that default to
                    // the useful version-family policy introduced in version 2.
                    const auto version = root["version"];
                    const bool currentVersion =
                            (version.isInt32() && version.getInt32() >= 2) ||
                            (version.isInt64() && version.getInt64() >= 2);
                    if (value == "hideExactDuplicates" && currentVersion)
                        return JsfxDuplicateListing::HideExactDuplicates;
                }
            } catch (...) {
                // A malformed optional setting falls back to the less noisy default.
            }
            return JsfxDuplicateListing::PreferLatestVersion;
        }

        void saveDuplicateListing(JsfxDuplicateListing value) {
            const auto file = settingsFile();
            if (file.empty())
                return;
            auto root = choc::value::createObject(
                    "JsfxSettings",
                    "version", static_cast<int64_t>(2),
                    "duplicateListing",
                    value == JsfxDuplicateListing::ShowAll ? "showAll" :
                    value == JsfxDuplicateListing::HideExactDuplicates
                        ? "hideExactDuplicates" : "preferLatestVersion");
            std::ofstream stream{file, std::ios::trunc};
            if (stream)
                stream << choc::json::toString(root, true);
        }

        std::optional<std::string> readFile(const std::filesystem::path& path) {
            std::ifstream stream{path, std::ios::binary};
            if (!stream)
                return {};
            return std::string{std::istreambuf_iterator<char>(stream),
                               std::istreambuf_iterator<char>()};
        }

        std::vector<std::string> importNames(const std::string& source) {
            std::vector<std::string> imports{};
            std::istringstream lines{source};
            std::string line;
            while (std::getline(lines, line)) {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (line.starts_with("@"))
                    break;
                if (!line.starts_with("import") || line.size() == 6 ||
                    !std::isspace(static_cast<unsigned char>(line[6])))
                    continue;
                auto begin = line.find_first_not_of(" \t", 7);
                if (begin == std::string::npos)
                    continue;
                auto end = line.find_last_not_of(" \t");
                imports.emplace_back(line.substr(begin, end - begin + 1));
            }
            return imports;
        }

        void appendField(std::string& signature, const std::string& value) {
            signature += std::to_string(value.size());
            signature.push_back(':');
            signature += value;
        }

        bool appendSourceClosure(ysfx_t* resolver,
                                 const std::filesystem::path& path,
                                 std::set<std::filesystem::path>& visited,
                                 std::string& signature) {
            std::error_code ec{};
            auto canonical = std::filesystem::weakly_canonical(path, ec);
            if (ec)
                return false;
            if (!visited.insert(canonical).second)
                return true;

            auto source = readFile(canonical);
            if (!source)
                return false;
            appendField(signature, *source);

            for (const auto& name : importNames(*source)) {
                char* resolved = ysfx_resolve_path_and_allocate(
                        resolver, name.c_str(), canonical.string().c_str());
                if (!resolved)
                    return false;
                std::filesystem::path imported{resolved};
                ysfx_free_resolved_path(resolved);
                if (!appendSourceClosure(resolver, imported, visited, signature))
                    return false;
            }
            return true;
        }

        std::optional<std::string> contentSignature(const std::filesystem::path& path) {
            ysfx_config_u config{ysfx_config_new()};
            if (!config)
                return {};
            ysfx_guess_file_roots(config.get(), path.string().c_str());
            ysfx_u resolver{ysfx_new(config.get())};
            if (!resolver)
                return {};

            std::set<std::filesystem::path> visited{};
            std::string signature{};
            if (!appendSourceClosure(resolver.get(), path, visited, signature))
                return {};
            return signature;
        }

        std::string duplicateCandidateKey(
                const uapmd_plugin_hosting::AudioPluginCatalogEntry& entry) {
            // Computing dependency closures is deliberately limited to plausible copies.
            // Exact copies have the same parsed metadata and leaf name; unrelated effects
            // are never compared or grouped merely because their source happens to match.
            return entry.displayName() + '\0' + entry.vendorName() + '\0' +
                   entry.bundlePath().filename().string();
        }

        struct SemanticVersion {
            uint64_t major{};
            uint64_t minor{};
            uint64_t patch{};
            std::string prerelease{};
        };

        bool parseVersionNumber(std::string_view text, uint64_t& value) {
            if (text.empty())
                return false;
            const auto* begin = text.data();
            const auto* end = begin + text.size();
            auto result = std::from_chars(begin, end, value);
            return result.ec == std::errc{} && result.ptr == end;
        }

        std::optional<SemanticVersion> parseSemanticVersion(std::string_view text) {
            if (!text.empty() && (text.front() == 'v' || text.front() == 'V'))
                text.remove_prefix(1);
            if (auto build = text.find('+'); build != std::string_view::npos)
                text = text.substr(0, build);

            std::string_view prerelease{};
            if (auto dash = text.find('-'); dash != std::string_view::npos) {
                prerelease = text.substr(dash + 1);
                text = text.substr(0, dash);
                if (prerelease.empty())
                    return {};
            }

            const auto firstDot = text.find('.');
            if (firstDot == std::string_view::npos)
                return {};
            const auto secondDot = text.find('.', firstDot + 1);
            if (secondDot == std::string_view::npos ||
                text.find('.', secondDot + 1) != std::string_view::npos)
                return {};

            SemanticVersion version{};
            if (!parseVersionNumber(text.substr(0, firstDot), version.major) ||
                !parseVersionNumber(text.substr(firstDot + 1, secondDot - firstDot - 1),
                                    version.minor) ||
                !parseVersionNumber(text.substr(secondDot + 1), version.patch))
                return {};
            version.prerelease = prerelease;
            return version;
        }

        int comparePrereleaseIdentifiers(std::string_view lhs, std::string_view rhs) {
            size_t lhsStart = 0;
            size_t rhsStart = 0;
            while (lhsStart < lhs.size() || rhsStart < rhs.size()) {
                if (lhsStart >= lhs.size())
                    return -1;
                if (rhsStart >= rhs.size())
                    return 1;
                const auto lhsEnd = lhs.find('.', lhsStart);
                const auto rhsEnd = rhs.find('.', rhsStart);
                const auto lhsPart = lhs.substr(
                        lhsStart, lhsEnd == std::string_view::npos ? lhs.size() - lhsStart
                                                                  : lhsEnd - lhsStart);
                const auto rhsPart = rhs.substr(
                        rhsStart, rhsEnd == std::string_view::npos ? rhs.size() - rhsStart
                                                                  : rhsEnd - rhsStart);
                uint64_t lhsNumber{};
                uint64_t rhsNumber{};
                const bool lhsNumeric = parseVersionNumber(lhsPart, lhsNumber);
                const bool rhsNumeric = parseVersionNumber(rhsPart, rhsNumber);
                if (lhsNumeric != rhsNumeric)
                    return lhsNumeric ? -1 : 1;
                if (lhsNumeric && lhsNumber != rhsNumber)
                    return lhsNumber < rhsNumber ? -1 : 1;
                if (!lhsNumeric && lhsPart != rhsPart)
                    return lhsPart < rhsPart ? -1 : 1;
                lhsStart = lhsEnd == std::string_view::npos ? lhs.size() : lhsEnd + 1;
                rhsStart = rhsEnd == std::string_view::npos ? rhs.size() : rhsEnd + 1;
            }
            return 0;
        }

        int compareVersions(const SemanticVersion& lhs, const SemanticVersion& rhs) {
            if (lhs.major != rhs.major)
                return lhs.major < rhs.major ? -1 : 1;
            if (lhs.minor != rhs.minor)
                return lhs.minor < rhs.minor ? -1 : 1;
            if (lhs.patch != rhs.patch)
                return lhs.patch < rhs.patch ? -1 : 1;
            if (lhs.prerelease.empty() != rhs.prerelease.empty())
                return lhs.prerelease.empty() ? 1 : -1;
            return comparePrereleaseIdentifiers(lhs.prerelease, rhs.prerelease);
        }

        struct VersionedFamily {
            std::string key{};
            SemanticVersion version{};
        };

        std::optional<VersionedFamily> versionedFamily(
                const uapmd_plugin_hosting::AudioPluginCatalogEntry& entry) {
            const auto& id = entry.pluginId();
            std::string key{};
            key.reserve(id.size());
            size_t start = 0;
            bool foundVersion = false;
            SemanticVersion version{};
            while (start <= id.size()) {
                const auto end = id.find('/', start);
                const auto component = std::string_view{id}.substr(
                        start, end == std::string::npos ? id.size() - start : end - start);
                if (!foundVersion) {
                    if (auto parsed = parseSemanticVersion(component)) {
                        foundVersion = true;
                        version = std::move(*parsed);
                        key += "{version}";
                    } else {
                        key.append(component);
                    }
                } else {
                    key.append(component);
                }
                if (end == std::string::npos)
                    break;
                key.push_back('/');
                start = end + 1;
            }
            if (!foundVersion)
                return {};
            // Metadata prevents coincidental, structurally similar paths from merging
            // when they advertise different effects.
            key.push_back('\0');
            key += entry.displayName();
            key.push_back('\0');
            key += entry.vendorName();
            return VersionedFamily{std::move(key), std::move(version)};
        }

        size_t pathComponentCount(const std::string& value) {
            return static_cast<size_t>(std::count(value.begin(), value.end(), '/'));
        }

        bool preferForListing(const uapmd_plugin_hosting::AudioPluginCatalogEntry& candidate,
                              const uapmd_plugin_hosting::AudioPluginCatalogEntry& current) {
            const auto candidateParts = pathComponentCount(candidate.pluginId());
            const auto currentParts = pathComponentCount(current.pluginId());
            if (candidateParts != currentParts)
                return candidateParts < currentParts;
            if (candidate.pluginId().size() != current.pluginId().size())
                return candidate.pluginId().size() < current.pluginId().size();
            return candidate.pluginId() < current.pluginId();
        }
    }

    void setJsfxEditorFontData(const void* data, size_t size) {
#if UAPMD_JSFX_OWN_FONTS
        setJsfxFontData(data, size);
#else
        // The platform's own fonts are in use; there is nothing to supply.
        (void) data;
        (void) size;
#endif
    }

    bool jsfxUsesOwnFontBackend() {
#if UAPMD_JSFX_OWN_FONTS
        return true;
#else
        return false;
#endif
    }

    struct JsfxPluginFormat::Impl {
        JsfxScanning scanning{};
        JsfxDuplicateListing duplicate_listing{loadDuplicateListing()};
    };

    JsfxPluginFormat::JsfxPluginFormat() : impl_(std::make_unique<Impl>()) {
    }

    JsfxPluginFormat::~JsfxPluginFormat() = default;

    std::string JsfxPluginFormat::name() {
        return kFormatName;
    }

    uapmd_plugin_hosting::AudioPluginUIThreadRequirement JsfxPluginFormat::requiresUIThreadOn(
            uapmd_plugin_hosting::AudioPluginCatalogEntry* entry) {
        (void) entry;
        return uapmd_plugin_hosting::UIThreadNotRequired;
    }

    uapmd_plugin_hosting::AudioPluginScanning* JsfxPluginFormat::scanning() {
        return &impl_->scanning;
    }

    void JsfxPluginFormat::createInstance(
            uapmd_plugin_hosting::AudioPluginCatalogEntry* entry,
            const uapmd_plugin_hosting::AudioPluginInstantiationOptions& options,
            std::function<void(std::unique_ptr<uapmd_plugin_hosting::AudioPluginInstanceAPI> instance,
                               std::string error)> callback) {
        if (!callback)
            return;
        if (!entry) {
            callback(nullptr, "no plugin was named");
            return;
        }

        // The catalog remembers where the effect was when it was scanned, but a project
        // may outlive that: search paths change, and effects move between machines. The
        // plugin id is the portable name, so it is what we resolve against the current
        // search paths, falling back to the remembered location.
        auto path = impl_->scanning.resolvePluginId(entry->pluginId());
        if (!path) {
            auto& remembered = entry->bundlePath();
            std::error_code ec{};
            if (!remembered.empty() && std::filesystem::is_regular_file(remembered, ec))
                path = remembered;
        }
        if (!path) {
            callback(nullptr, "could not find the JSFX effect " + entry->pluginId());
            return;
        }

        // JSFX has no main thread affinity and compiling a script is not slow, so this
        // completes before returning rather than deferring to a loader thread.
        std::string error{};
        auto instance = JsfxPluginInstance::create(*path, entry->pluginId(), options, error);
        if (!instance) {
            callback(nullptr, error.empty() ? "could not load the JSFX effect" : error);
            return;
        }
        callback(std::move(instance), "");
    }

    void JsfxPluginFormat::searchPaths(std::vector<std::string> paths, bool alsoUseDefaults) {
        auto& overrides = impl_->scanning.getOverrideSearchPaths();
        overrides.clear();
        for (auto& path : paths)
            overrides.emplace_back(std::move(path));
        impl_->scanning.useDefaultSearchPaths(alsoUseDefaults);
    }

    std::vector<std::string> JsfxPluginFormat::searchPaths() const {
        return impl_->scanning.getOverrideSearchPaths();
    }

    bool JsfxPluginFormat::useDefaultSearchPaths() const {
        return impl_->scanning.useDefaultSearchPaths();
    }

    std::vector<std::filesystem::path> JsfxPluginFormat::defaultSearchPaths() const {
        return impl_->scanning.getDefaultSearchPaths();
    }

    JsfxDuplicateListing JsfxPluginFormat::duplicateListing() const {
        return impl_->duplicate_listing;
    }

    void JsfxPluginFormat::duplicateListing(JsfxDuplicateListing value) {
        if (impl_->duplicate_listing == value)
            return;
        impl_->duplicate_listing = value;
        saveDuplicateListing(value);
    }

    void JsfxPluginFormat::filterPluginList(
            std::vector<uapmd_plugin_hosting::AudioPluginCatalogEntry>& entries) const {
        if (impl_->duplicate_listing == JsfxDuplicateListing::ShowAll)
            return;

        std::vector<bool> hidden(entries.size(), false);
        if (impl_->duplicate_listing == JsfxDuplicateListing::PreferLatestVersion) {
            struct Representative {
                size_t index{};
                SemanticVersion version{};
            };
            std::unordered_map<std::string, Representative> representativeByFamily{};
            for (size_t i = 0; i < entries.size(); i++) {
                if (entries[i].format() != kFormatName)
                    continue;
                auto family = versionedFamily(entries[i]);
                if (!family)
                    continue;
                auto [it, inserted] = representativeByFamily.try_emplace(
                        family->key, Representative{i, family->version});
                if (inserted)
                    continue;
                const auto comparison = compareVersions(family->version, it->second.version);
                if (comparison > 0 ||
                    comparison == 0 && preferForListing(entries[i], entries[it->second.index])) {
                    hidden[it->second.index] = true;
                    it->second = Representative{i, std::move(family->version)};
                } else {
                    hidden[i] = true;
                }
            }
        }

        std::unordered_map<std::string, std::vector<size_t>> candidates{};
        for (size_t i = 0; i < entries.size(); i++)
            if (!hidden[i] && entries[i].format() == kFormatName)
                candidates[duplicateCandidateKey(entries[i])].push_back(i);

        for (const auto& [key, indices] : candidates) {
            (void) key;
            if (indices.size() < 2)
                continue;

            std::unordered_map<std::string, size_t> representativeBySignature{};
            for (auto index : indices) {
                auto signature = contentSignature(entries[index].bundlePath());
                if (!signature)
                    continue;
                auto [it, inserted] = representativeBySignature.try_emplace(*signature, index);
                if (inserted)
                    continue;
                auto current = it->second;
                if (preferForListing(entries[index], entries[current])) {
                    hidden[current] = true;
                    it->second = index;
                } else {
                    hidden[index] = true;
                }
            }
        }

        std::vector<uapmd_plugin_hosting::AudioPluginCatalogEntry> visible{};
        visible.reserve(entries.size());
        for (size_t i = 0; i < entries.size(); i++)
            if (!hidden[i])
                visible.emplace_back(std::move(entries[i]));
        entries = std::move(visible);
    }

}
