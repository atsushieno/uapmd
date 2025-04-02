#pragma once

namespace remidy {
    class PluginScanning {
    public:
        PluginScanning() = default;

        // Returns true if scanRequiresLoadLibrary() does not return `ScanningStrategyValue::NEVER`.
        // Usually this means that you should resort to plugin list cache.
        bool scanningMayBeSlow();

        enum class ScanningStrategyValue {
            NEVER,
            MAYBE,
            ALWAYS
        };
        // Indicates whether scanning of the plugin requires loading of the library.
        // `ALWAYS` for CLAP, `MAYBE` for VST3 (when `moduleinfo.json` does not exist), `NONE` for AU and LV2.
        virtual ScanningStrategyValue scanRequiresLoadLibrary() = 0;
        // Indicates whether scanning of the plugin requires instancing of the plugin IF it needs to load the library.
        // NEVER for AU, LV2, and CLAP. ALWAYS for VST3.
        virtual ScanningStrategyValue scanRequiresInstantiation() = 0;
        // Implements plugin scanning. You might want to use this functionality in a separate process as
        // some bad behaving plugins may crash the process.
        virtual std::vector<std::unique_ptr<PluginCatalogEntry>> scanAllAvailablePlugins() = 0;

    };

    // Desktop specific plugin format members.
    class FileBasedPluginScanning : public PluginScanning {
        // Indicates that scanning of the plugins in this format is based on file paths (VST3, LV2, CLAP vs. AU).
        virtual bool usePluginSearchPaths() = 0;
        // Provides the default search paths for the format, if its plugin scanning is file-based.
        virtual std::vector<std::filesystem::path>& getDefaultSearchPaths() = 0;

        std::vector<std::string> overrideSearchPaths{};

        std::vector<std::string>& getOverrideSearchPaths() { return overrideSearchPaths; }
        void addSearchPath(const std::string& path) { overrideSearchPaths.emplace_back(path); }
    };
}
