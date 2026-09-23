#pragma once

#include <array>
#include <vector>
#include <string>
#include <functional>
#include <unordered_set>
#include <imgui.h>
#include <PluginUIHelpers.hpp>

namespace uapmd_app_gui {

class PluginList {
public:
    struct Selection {
        std::string format;
        std::string pluginId;
        bool hasSelection = false;
    };

    enum class GroupMode {
        None,
        Format,
        Developer,
        Bundle
    };

private:
    enum class Column {
        Format,
        Name,
        Vendor,
        Id,
        Bundle
    };

    struct Group {
        std::string key;
        std::string label;
        std::string tooltip;
        bool placeholder = false;
        std::vector<int> members;
    };

    struct Row {
        bool isGroup = false;
        int index = 0;
    };

    struct SortSpec {
        int column = 0;
        ImGuiSortDirection direction = ImGuiSortDirection_Ascending;
    };

    std::vector<remidy_imgui::PluginEntry> availablePlugins_{};
    std::vector<std::string> lowerNames_{};
    std::vector<std::string> lowerVendors_{};
    std::vector<std::string> bundleLabels_{};
    std::string selectedPluginFormat_;
    std::string selectedPluginId_;
    char searchFilter_[256] = "";

    GroupMode groupMode_ = GroupMode::None;
    bool groupSortDescending_ = false;
    std::array<std::unordered_set<std::string>, 4> expandedGroups_{};
    std::unordered_set<std::string> searchCollapsedGroups_{};
    std::vector<SortSpec> flatSortSpecs_{};
    std::vector<Group> groups_{};
    std::vector<Row> rows_{};
    bool rowsDirty_ = true;

    std::function<void(const std::string& format, const std::string& pluginId, const std::string& name)> onPluginSelected_;

public:
    PluginList();

    void setPlugins(const std::vector<remidy_imgui::PluginEntry>& plugins);
    // reservedHeight keeps room below the table for the caller's own controls
    void render(float reservedHeight = 0.0f);

    void setOnPluginSelected(std::function<void(const std::string& format, const std::string& pluginId, const std::string& name)> callback);

    Selection getSelection() const;
    void clearSelection();
    void setSearchFilter(const char* filter);
    const char* getSearchFilter() const;

    GroupMode groupMode() const { return groupMode_; }
    void groupMode(GroupMode mode);
    void expandAllGroups();
    void collapseAllGroups();

private:
    void renderToolbar();
    void renderTable(float reservedHeight);
    void renderGroupRow(int groupIndex, bool& toggled);
    void renderPluginRow(int pluginIndex, const std::vector<Column>& columns, bool indent);
    void renderCell(Column column, int pluginIndex);
    std::vector<Column> activeColumns() const;
    bool searchActive() const { return searchFilter_[0] != '\0'; }
    bool isGroupExpanded(const std::string& key) const;
    void setGroupExpanded(const std::string& key, bool expanded);
    void rebuildRows();
    std::vector<int> filterIndices() const;
    void sortFlat(std::vector<int>& indices) const;
    void buildGroups(const std::vector<int>& indices);
};

}
