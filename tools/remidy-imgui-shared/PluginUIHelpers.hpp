#pragma once

#include <string>
#include <vector>
#include <algorithm>
#include <cctype>

namespace uapmd::gui {

/**
 * Represents a plugin available for loading.
 * Shared structure used by plugin selectors across different tools.
 */
struct PluginEntry {
    std::string format;      // e.g., "VST3", "CLAP", "AU"
    std::string id;          // Plugin ID (VST3 UID, CLAP ID, etc.)
    std::string name;        // Display name
    std::string vendor;      // Manufacturer/vendor name
};

/**
 * Plugin selector filtering utilities
 */
namespace PluginFiltering {

    /**
     * Convert string to lowercase for case-insensitive comparison
     */
    inline std::string toLower(std::string str) {
        std::transform(str.begin(), str.end(), str.begin(),
                      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return str;
    }

    /**
     * Check if a plugin matches the given search filter
     */
    inline bool matchesFilter(const PluginEntry& plugin, const std::string& filter) {
        if (filter.empty()) return true;

        std::string lowerFilter = toLower(filter);
        return toLower(plugin.name).find(lowerFilter) != std::string::npos ||
               toLower(plugin.vendor).find(lowerFilter) != std::string::npos ||
               toLower(plugin.format).find(lowerFilter) != std::string::npos ||
               toLower(plugin.id).find(lowerFilter) != std::string::npos;
    }

    /**
     * Sort plugins by a specific column
     */
    enum class SortColumn {
        Format,
        Name,
        Vendor,
        Id
    };

    inline void sortPlugins(std::vector<PluginEntry>& plugins, SortColumn column, bool ascending) {
        auto comparator = [column, ascending](const PluginEntry& a, const PluginEntry& b) {
            int result = 0;
            switch (column) {
                case SortColumn::Format:
                    result = a.format.compare(b.format);
                    break;
                case SortColumn::Name:
                    result = a.name.compare(b.name);
                    break;
                case SortColumn::Vendor:
                    result = a.vendor.compare(b.vendor);
                    break;
                case SortColumn::Id:
                    result = a.id.compare(b.id);
                    break;
            }
            return ascending ? (result < 0) : (result > 0);
        };
        std::sort(plugins.begin(), plugins.end(), comparator);
    }

} // namespace PluginFiltering

} // namespace uapmd::gui
