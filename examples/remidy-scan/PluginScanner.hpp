#include <remidy/remidy.hpp>
#include <cpplocate/cpplocate.h>

#pragma once

namespace remidy {

    class PluginScanner {

        // -------- scanning --------

        std::vector<std::string> vst3SearchPaths{};
        std::vector<std::string> lv2SearchPaths{};
        AudioPluginFormatVST3 vst3{vst3SearchPaths};
        AudioPluginFormatAU au{};
        AudioPluginFormatLV2 lv2{lv2SearchPaths};
        std::filesystem::path pluginListCacheFile{};
        std::string app_name;

    public:
        std::vector<AudioPluginFormat*> formats{&lv2, &au, &vst3};
        PluginCatalog catalog{};
        auto filterByFormat(std::vector<PluginCatalogEntry*> entries, std::string format) {
            erase_if(entries, [format](PluginCatalogEntry* entry) { return entry->format() != format; });
            return entries;
        }

        explicit PluginScanner(std::string appName) : app_name(appName) {}
        ~PluginScanner() = default;

        int performPluginScanning();

        void savePluginListCache() {
            if (!pluginListCacheFile.empty()) {
                catalog.save(pluginListCacheFile);
                //std::cerr << "Saved plugin list cache as " << scanner.pluginListCacheFile << std::endl;
            }
        }

        bool safeToInstantiate(AudioPluginFormat* format, PluginCatalogEntry* entry);

        bool createInstanceOnUIThread(AudioPluginFormat* format, PluginCatalogEntry* entry);
    };
}
