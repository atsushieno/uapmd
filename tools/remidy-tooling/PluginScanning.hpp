#include <remidy/remidy.hpp>
#include <cpplocate/cpplocate.h>

#pragma once

namespace remidy_tooling {
    using namespace remidy;

    class PluginScanning {

        // -------- scanning --------

        std::vector<std::string> vst3SearchPaths{};
        std::vector<std::string> lv2SearchPaths{};
        PluginFormatVST3 vst3{vst3SearchPaths};
        PluginFormatAU au{};
        PluginFormatLV2 lv2{lv2SearchPaths};
        std::filesystem::path pluginListCacheFile{};

    public:
        std::vector<PluginFormat*> formats{&lv2, &au, &vst3};
        PluginCatalog catalog{};
        auto filterByFormat(std::vector<PluginCatalogEntry*> entries, std::string format) {
            erase_if(entries, [format](PluginCatalogEntry* entry) { return entry->format() != format; });
            return entries;
        }

        ~PluginScanning() = default;

        int performPluginScanning(std::filesystem::path& pluginListCacheFile);

        void savePluginListCache(std::filesystem::path& fileToSave) {
            catalog.save(fileToSave);
        }

        bool safeToInstantiate(PluginFormat* format, PluginCatalogEntry* entry);

        bool shouldCreateInstanceOnUIThread(PluginFormat* format, PluginCatalogEntry* entry);
    };
}