#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "uapmd-plugin-hosting/uapmd-plugin-hosting.hpp"

namespace uapmd_jsfx {

    inline constexpr const char* kFormatName = "JSFX";

    // Finds the JSFX effects installed on this machine.
    //
    // JSFX effects are source files, so discovering one means reading it rather than
    // loading a binary: no dlopen, no compilation, and no script code ever runs during a
    // scan. That makes the whole scan fast enough to redo on every launch, which is why
    // this reports scanningMayBeSlow() == false and returns everything from
    // getAllFastScannablePlugins(). The host's blocklist, per-bundle timeout and
    // out-of-process scanner never come into play.
    //
    // AudioPluginFileOrUrlScanning is implemented for its search path API rather than
    // because scanning is slow. Users keep effects in more places than REAPER's own folder.
    class JsfxScanning : public uapmd_plugin_hosting::AudioPluginFileOrUrlScanning {
        std::vector<std::filesystem::path> default_search_paths_{};

    public:
        JsfxScanning();

        bool scanningMayBeSlow() override { return false; }
        ScanningStrategy scanRequiresLoadLibrary() override { return ScanningStrategy::Never; }
        bool scanRequiresLoadLibrary(const std::filesystem::path& bundlePath) override {
            (void) bundlePath;
            return false;
        }
        ScanningStrategy scanRequiresInstantiation() override { return ScanningStrategy::Never; }

        std::vector<uapmd_plugin_hosting::AudioPluginCatalogEntry> getAllFastScannablePlugins() override;

        void startSlowPluginScan(uapmd_plugin_hosting::AudioPluginFoundCallback pluginFound,
                                 uapmd_plugin_hosting::AudioPluginScanCompletedCallback scanCompleted) override;

        bool usePluginSearchPaths() override { return true; }
        std::vector<std::filesystem::path>& getDefaultSearchPaths() override { return default_search_paths_; }

        std::vector<std::filesystem::path> enumerateCandidateBundles(bool requireFastScanning) override;

        void scanBundle(const std::filesystem::path& bundlePath,
                        bool requireFastScanning,
                        double timeoutSeconds,
                        uapmd_plugin_hosting::AudioPluginFoundCallback pluginFound,
                        uapmd_plugin_hosting::AudioPluginScanCompletedCallback scanCompleted) override;

        // The roots that are searched, honouring useDefaultSearchPaths().
        std::vector<std::filesystem::path> activeSearchRoots() const;

        // Resolves a plugin id back to the file it names, by trying each search root in
        // turn. Returns nothing when no root holds it, which happens when a project refers
        // to an effect this machine does not have.
        std::optional<std::filesystem::path> resolvePluginId(const std::string& pluginId) const;
    };

    // Reads one file and describes the effect in it, or returns nothing when the file is
    // not a JSFX effect. `pluginId` is the path relative to the search root it came from.
    //
    // Validity is decided by ysfx's own parser rather than by looking for a `desc:` line:
    // JSFX in the wild is twenty years of inconsistent formatting, CRLF and non-UTF-8 text,
    // and hand-written sniffing gets it wrong. An effect is anything that parses and either
    // names itself or has code in it.
    std::optional<uapmd_plugin_hosting::AudioPluginCatalogEntry> describeJsfxFile(
            const std::filesystem::path& path, const std::string& pluginId);

}
