#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace remidy {
    class PluginCatalogEntry;
    using PluginScanCompletedCallback = std::function<void(std::string error)>;

    class PluginScanning {
    public:
        virtual ~PluginScanning() = default;

        enum class ScanningStrategyValue {
            NEVER,
            MAYBE,
            ALWAYS
        };

        // Indicates some plugins MAY be slow, so without any plugin list cache the plugin list will be empty.
        // VST3 (sometimes slow), CLAP (always slow), and WebCLAP (always slow) return true.
        // AU, LV2, and AAP return false.
        virtual bool scanningMayBeSlow() = 0;

        // Indicates whether scanning of the plugin requires loading of the library.
        // `ALWAYS` for CLAP, `MAYBE` for VST3 (when `moduleinfo.json` does not exist), `NONE` for AU and LV2.
        virtual ScanningStrategyValue scanRequiresLoadLibrary() = 0;

        // Indicates whether scanning of the specific bundle requires loading of the library.
        virtual bool scanRequiresLoadLibrary(const std::filesystem::path& bundlePath) = 0;

        // Indicates whether scanning of the plugin requires instancing of the plugin IF it needs to load the library.
        // NEVER for AU, LV2, and CLAP. ALWAYS for VST3.
        virtual ScanningStrategyValue scanRequiresInstantiation() = 0;

        // Implements fast plugin scanning.
        // Partially fast plugin scanners can return fast-scannable plugins.
        // With slow scanning results, the list will become complete.
        virtual std::vector<PluginCatalogEntry> getAllFastScannablePlugins() = 0;

        // Implements slow plugin scanning. It is asynchronously achieved with the callbacks.
        // You might want to use this functionality in a separate process as some plugin factories may crash the process.
        virtual void startSlowPluginScan(std::function<void(PluginCatalogEntry entry)> pluginFound,
                                         PluginScanCompletedCallback scanCompleted) = 0;
    };

    // Desktop specific plugin format members.
    class FileOrUrlBasedPluginScanning : public PluginScanning {
        std::vector<std::string> overrideSearchPaths{};

    public:
        // Indicates that scanning of the plugins in this format is based on file paths or URL-like bundle identifiers
        // (VST3, CLAP, WebCLAP vs. AU).
        virtual bool usePluginSearchPaths() = 0;
        // Provides the default search paths for the format, if its plugin scanning is path-based.
        virtual std::vector<std::filesystem::path>& getDefaultSearchPaths() = 0;

        std::vector<std::string>& getOverrideSearchPaths() { return overrideSearchPaths; }
        void addSearchPath(const std::string& path) { overrideSearchPaths.emplace_back(path); }

        // Scans plugins contained within the specific bundle.
        virtual void scanBundle(const std::filesystem::path& bundlePath,
                                bool requireFastScanning,
                                double timeoutSeconds,
                                std::function<void(PluginCatalogEntry entry)> pluginFound,
                                PluginScanCompletedCallback scanCompleted) = 0;
        virtual std::vector<std::filesystem::path> enumerateCandidateBundles(bool requireFastScanning) = 0;
    };
}
