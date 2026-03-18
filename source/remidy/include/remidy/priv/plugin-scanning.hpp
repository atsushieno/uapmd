#pragma once

#include <filesystem>
#include <memory>
#include <vector>

namespace remidy {
    class PluginCatalogEntry;
    class PluginScanning {
    public:
        virtual ~PluginScanning() = default;

        enum class ScanningStrategyValue {
            NEVER,
            MAYBE,
            ALWAYS
        };
        // Indicates whether scanning of the plugin requires loading of the library.
        // `ALWAYS` for CLAP, `MAYBE` for VST3 (when `moduleinfo.json` does not exist), `NONE` for AU and LV2.
        virtual ScanningStrategyValue scanRequiresLoadLibrary() = 0;
        // Indicates whether scanning of the specific bundle requires loading of the library.
        virtual bool scanRequiresLoadLibrary(const std::filesystem::path& bundlePath) = 0;
        // Indicates whether scanning of the plugin requires instancing of the plugin IF it needs to load the library.
        // NEVER for AU, LV2, and CLAP. ALWAYS for VST3.
        virtual ScanningStrategyValue scanRequiresInstantiation() = 0;
        // Implements plugin scanning. You might want to use this functionality in a separate process as
        // some bad behaving plugins may crash the process.
        virtual std::vector<std::unique_ptr<PluginCatalogEntry>> scanAllAvailablePlugins(bool requireFastScanning) = 0;
    };

    // Desktop specific plugin format members.
    class FileBasedPluginScanning : public PluginScanning {
        std::vector<std::string> overrideSearchPaths{};

    public:
        // Indicates that scanning of the plugins in this format is based on file paths (VST3, LV2, CLAP vs. AU).
        virtual bool usePluginSearchPaths() = 0;
        // Provides the default search paths for the format, if its plugin scanning is file-based.
        virtual std::vector<std::filesystem::path>& getDefaultSearchPaths() = 0;

        std::vector<std::string>& getOverrideSearchPaths() { return overrideSearchPaths; }
        void addSearchPath(const std::string& path) { overrideSearchPaths.emplace_back(path); }

        // Scans plugins contained within the specific bundle.
        virtual std::vector<std::unique_ptr<PluginCatalogEntry>> scanBundle(const std::filesystem::path& bundlePath,
                                                                            bool requireFastScanning,
                                                                            double timeoutSeconds) = 0;
        virtual std::vector<std::filesystem::path> enumerateCandidateBundles(bool requireFastScanning) = 0;
    };
}
