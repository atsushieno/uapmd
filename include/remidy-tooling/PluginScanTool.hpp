#pragma once

#include "remidy/remidy.hpp"

namespace remidy_tooling {
    using namespace remidy;

    class PluginScanTool {

        // -------- scanning --------

        std::vector<std::string> vst3SearchPaths{};
        std::vector<std::string> lv2SearchPaths{};
        std::vector<std::string> clapSearchPaths{};
        PluginFormatVST3 vst3{vst3SearchPaths};
#if __APPLE__
        PluginFormatAU au{};
#endif
        PluginFormatLV2 lv2{lv2SearchPaths};
        PluginFormatCLAP clap{clapSearchPaths};
        std::filesystem::path plugin_list_cache_file{};

        std::vector<PluginFormat*> formats_{
            &clap,
            &lv2,
#if __APPLE__
            &au,
#endif
            &vst3};
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

        bool safeToInstantiate(PluginFormat* format, PluginCatalogEntry* entry);

        bool shouldCreateInstanceOnUIThread(PluginFormat* format, PluginCatalogEntry* entry);
    };
}
