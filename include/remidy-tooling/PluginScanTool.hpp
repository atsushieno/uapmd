#pragma once

#include "remidy/remidy.hpp"

namespace remidy_tooling {
    using namespace remidy;

    class PluginScanTool {

        // -------- scanning --------

        std::vector<std::string> vst3SearchPaths{};
        std::vector<std::string> lv2SearchPaths{};
        std::vector<std::string> clapSearchPaths{};
        std::unique_ptr<PluginFormatVST3> vst3;
        std::unique_ptr<PluginFormatLV2> lv2;
        std::unique_ptr<PluginFormatCLAP> clap;
#if __APPLE__
        std::unique_ptr<PluginFormatAU> au;
#endif
        std::filesystem::path plugin_list_cache_file{};

        std::vector<PluginFormat*> formats_;
    public:
        PluginScanTool();

        PluginCatalog catalog{};

        auto filterByFormat(std::vector<PluginCatalogEntry*> entries, std::string format) {
            erase_if(entries, [format](PluginCatalogEntry* entry) { return entry->format() != format; });
            return entries;
        }

        std::vector<PluginFormat*> formats() { return formats_; }
        void addFormat(PluginFormat* item) { formats_.emplace_back(item); }

        std::filesystem::path& pluginListCacheFile() { return plugin_list_cache_file; }
        int performPluginScanning();
        int performPluginScanning(std::filesystem::path& pluginListCacheFile);

        void savePluginListCache() { savePluginListCache(pluginListCacheFile()); }
        void savePluginListCache(std::filesystem::path& fileToSave) {
            catalog.save(fileToSave);
        }

        // We have some plugins that cause either freezes or runtime crashes that prevents our business logic.
        // That does not necessarily mean that the plugin cannot be instantiated in general. It's a scan-time-only thing.
        // We enumerate catalog entries but skip instancing validation.
        bool safeToInstantiate(PluginFormat* format, PluginCatalogEntry* entry);

        bool shouldCreateInstanceOnUIThread(PluginFormat* format, PluginCatalogEntry* entry);
    };
}
