#pragma once

#include <vector>
#include <string>
#include <functional>
#include <imgui.h>
#include <PluginUIHelpers.hpp>

namespace uapmd::gui {

class PluginList {
public:
    struct Selection {
        std::string format;
        std::string pluginId;
        bool hasSelection = false;
    };

private:
    std::vector<PluginEntry> availablePlugins_{};
    std::string selectedPluginFormat_;
    std::string selectedPluginId_;
    char searchFilter_[256] = "";

    std::function<void(const std::string& format, const std::string& pluginId, const std::string& name)> onPluginSelected_;

public:
    PluginList();

    void setPlugins(const std::vector<PluginEntry>& plugins);
    void render();

    void setOnPluginSelected(std::function<void(const std::string& format, const std::string& pluginId, const std::string& name)> callback);

    Selection getSelection() const;
    void clearSelection();
    void setSearchFilter(const char* filter);
    const char* getSearchFilter() const;

private:
    std::vector<int> filterAndBuildIndices();
    void sortIndices(std::vector<int>& indices);
};

}
