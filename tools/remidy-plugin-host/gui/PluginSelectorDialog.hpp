#pragma once

#include <vector>
#include <string>
#include <functional>
#include <GLFW/glfw3.h>
#include <uapmd/uapmd.hpp>

namespace uapmd::gui {
    struct PluginEntry {
        std::string format;
        std::string id;
        std::string name;
        std::string vendor;
    };

    class PluginSelectorDialog {
        bool isOpen_ = false;

        // Plugin data
        std::vector<PluginEntry> availablePlugins_;
        std::vector<PluginEntry> denyListPlugins_;
        std::string selectedPluginFormat_ = "";
        std::string selectedPluginId_ = "";
        char searchFilter_[256] = "";
        bool showDenyList_ = false;

        // Callback for when a plugin is selected
        std::function<void(const std::string& format, const std::string& id)> onPluginSelected_;

    public:
        PluginSelectorDialog();
        ~PluginSelectorDialog();

        void show(std::function<void(const std::string& format, const std::string& id)> callback);
        void hide();
        bool isVisible() const { return isOpen_; }
        void render();
        void refreshPluginList();
        void rescanPlugins();
        std::vector<PluginEntry> getFilteredPlugins() const;
        void instantiateSelectedPlugin();
    };
}