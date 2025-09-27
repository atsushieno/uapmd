#pragma once

#include <vector>
#include <string>

namespace uapmd::gui {
    struct PluginEntry {
        std::string format;
        std::string id;
        std::string name;
        std::string vendor;
    };

    class AudioPluginSelectorWindow {
        bool isOpen_ = true;
        std::vector<PluginEntry> availablePlugins_;
        std::vector<PluginEntry> denyListPlugins_;
        int selectedPlugin_ = -1;
        bool showDenyList_ = false;
        char searchFilter_[256] = {0};

    public:
        AudioPluginSelectorWindow();
        void render();

    private:
        void refreshPluginList();
        void instantiateSelectedPlugin();
        void rescanPlugins();
        std::vector<PluginEntry> getFilteredPlugins() const;
    };
}