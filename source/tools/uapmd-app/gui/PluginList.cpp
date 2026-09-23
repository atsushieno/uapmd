#include "PluginList.hpp"
#include <algorithm>
#include <iostream>
#include <cfloat>
#include <cstring>
#include <filesystem>
#include <unordered_map>


namespace uapmd_app_gui {

namespace {

// Case-insensitive for ASCII letters; non-ASCII compared by codepoint
int compareCI(const std::string& lhs, const std::string& rhs) {
    for (size_t i = 0, n = std::min(lhs.size(), rhs.size()); i < n; ++i) {
        auto a = static_cast<unsigned char>(lhs[i]);
        auto b = static_cast<unsigned char>(rhs[i]);
        if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
        if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
        if (a != b) return static_cast<int>(a) - static_cast<int>(b);
    }
    return static_cast<int>(lhs.size()) - static_cast<int>(rhs.size());
}

std::string trim(const std::string& s) {
    auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos)
        return {};
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

const char* groupModeLabel(PluginList::GroupMode mode) {
    switch (mode) {
        case PluginList::GroupMode::None: return "None";
        case PluginList::GroupMode::Format: return "Plugin Format";
        case PluginList::GroupMode::Developer: return "Developer";
        case PluginList::GroupMode::Bundle: return "Bundle";
    }
    return "";
}

}

PluginList::PluginList() {
    std::fill(std::begin(searchFilter_), std::end(searchFilter_), '\0');
}

void PluginList::setPlugins(const std::vector<remidy_imgui::PluginEntry>& plugins) {
    availablePlugins_ = plugins;
    lowerNames_.clear();
    lowerVendors_.clear();
    bundleLabels_.clear();
    lowerNames_.reserve(plugins.size());
    lowerVendors_.reserve(plugins.size());
    bundleLabels_.reserve(plugins.size());
    for (const auto& p : availablePlugins_) {
        lowerNames_.push_back(remidy_imgui::PluginFiltering::toLower(p.name));
        lowerVendors_.push_back(remidy_imgui::PluginFiltering::toLower(p.vendor));
        auto label = std::filesystem::path{p.bundle}.filename().string();
        bundleLabels_.push_back(label.empty() ? p.bundle : label);
    }
    rowsDirty_ = true;
}

void PluginList::render() {
    renderToolbar();
    renderTable();
}

void PluginList::renderToolbar() {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Group by:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::CalcTextSize("Plugin Format").x + ImGui::GetFrameHeight() * 2.0f);
    if (ImGui::BeginCombo("##PluginGroupMode", groupModeLabel(groupMode_))) {
        for (auto mode : {GroupMode::None, GroupMode::Format, GroupMode::Developer, GroupMode::Bundle}) {
            bool selected = mode == groupMode_;
            if (ImGui::Selectable(groupModeLabel(mode), selected))
                groupMode(mode);
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    if (groupMode_ != GroupMode::None) {
        ImGui::SameLine();
        if (ImGui::Button(groupSortDescending_ ? "Z-A" : "A-Z")) {
            groupSortDescending_ = !groupSortDescending_;
            rowsDirty_ = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Toggle sort order by name");
        ImGui::SameLine();
        if (ImGui::Button("Expand All"))
            expandAllGroups();
        ImGui::SameLine();
        if (ImGui::Button("Collapse All"))
            collapseAllGroups();
    }

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Search:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputText("##PluginSearch", searchFilter_, sizeof(searchFilter_))) {
        searchCollapsedGroups_.clear();
        rowsDirty_ = true;
    }
}

void PluginList::renderTable() {
    auto columns = activeColumns();
    bool grouped = groupMode_ != GroupMode::None;
    ImGuiTableFlags flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY;
    if (!grouped)
        flags |= ImGuiTableFlags_Sortable;

    ImGui::PushID(static_cast<int>(groupMode_));
    if (ImGui::BeginTable("PluginTable", static_cast<int>(columns.size()), flags, ImVec2(0, 300))) {
        for (auto column : columns) {
            switch (column) {
                case Column::Format: ImGui::TableSetupColumn("Format", ImGuiTableColumnFlags_WidthFixed, 80.0f); break;
                case Column::Name: ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch); break;
                case Column::Vendor: ImGui::TableSetupColumn("Vendor", ImGuiTableColumnFlags_WidthStretch); break;
                case Column::Id: ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthStretch); break;
                case Column::Bundle: ImGui::TableSetupColumn("Bundle", ImGuiTableColumnFlags_WidthStretch); break;
            }
        }
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        if (!grouped) {
            if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs(); specs && specs->SpecsDirty) {
                flatSortSpecs_.clear();
                for (int n = 0; n < specs->SpecsCount; n++)
                    flatSortSpecs_.push_back({specs->Specs[n].ColumnIndex, specs->Specs[n].SortDirection});
                specs->SpecsDirty = false;
                rowsDirty_ = true;
            }
        }

        if (rowsDirty_)
            rebuildRows();

        // Expansion changes alter the row list, so apply them after the clipper pass
        bool toggled = false;
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(rows_.size()));
        while (clipper.Step()) {
            for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
                const auto& row = rows_[static_cast<size_t>(r)];
                if (row.isGroup)
                    renderGroupRow(row.index, toggled);
                else
                    renderPluginRow(row.index, columns, grouped);
            }
        }
        if (toggled)
            rowsDirty_ = true;
        ImGui::EndTable();
    }
    ImGui::PopID();
}

void PluginList::renderGroupRow(int groupIndex, bool& toggled) {
    const auto& group = groups_[static_cast<size_t>(groupIndex)];
    bool expanded = isGroupExpanded(group.key);

    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::SetNextItemOpen(expanded, ImGuiCond_Always);
    bool open = ImGui::TreeNodeEx(group.placeholder ? "##placeholder" : group.key.c_str(),
                                  ImGuiTreeNodeFlags_SpanAllColumns | ImGuiTreeNodeFlags_NoTreePushOnOpen,
                                  "%s (%zu)", group.label.c_str(), group.members.size());
    if (!group.tooltip.empty() && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", group.tooltip.c_str());
    if (open != expanded) {
        setGroupExpanded(group.key, open);
        toggled = true;
    }
}

void PluginList::renderPluginRow(int pluginIndex, const std::vector<Column>& columns, bool indent) {
    const auto& plugin = availablePlugins_[static_cast<size_t>(pluginIndex)];
    bool isSelected = (selectedPluginFormat_ == plugin.format && selectedPluginId_ == plugin.id);

    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::PushID(pluginIndex);
    if (indent)
        ImGui::Indent(ImGui::GetTreeNodeToLabelSpacing());
    if (ImGui::Selectable("##row", isSelected, ImGuiSelectableFlags_SpanAllColumns)) {
        selectedPluginFormat_ = plugin.format;
        selectedPluginId_ = plugin.id;
        std::cout << "[GUI] Selected plugin: format='" << plugin.format << "', id='" << plugin.id
                  << "', name='" << plugin.name << "'" << std::endl;

        if (onPluginSelected_)
            onPluginSelected_(plugin.format, plugin.id, plugin.name);
    }
    ImGui::SameLine();
    renderCell(columns[0], pluginIndex);
    if (indent)
        ImGui::Unindent(ImGui::GetTreeNodeToLabelSpacing());
    ImGui::PopID();

    for (size_t c = 1; c < columns.size(); ++c) {
        ImGui::TableNextColumn();
        renderCell(columns[c], pluginIndex);
    }
}

void PluginList::renderCell(Column column, int pluginIndex) {
    const auto& plugin = availablePlugins_[static_cast<size_t>(pluginIndex)];
    switch (column) {
        case Column::Format: ImGui::TextUnformatted(plugin.format.c_str()); break;
        case Column::Name: ImGui::TextUnformatted(plugin.name.c_str()); break;
        case Column::Vendor: ImGui::TextUnformatted(plugin.vendor.c_str()); break;
        case Column::Id: ImGui::TextUnformatted(plugin.id.c_str()); break;
        case Column::Bundle:
            ImGui::TextUnformatted(bundleLabels_[static_cast<size_t>(pluginIndex)].c_str());
            if (plugin.bundle != bundleLabels_[static_cast<size_t>(pluginIndex)] && ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", plugin.bundle.c_str());
            break;
    }
}

std::vector<PluginList::Column> PluginList::activeColumns() const {
    switch (groupMode_) {
        case GroupMode::None: return {Column::Format, Column::Name, Column::Vendor, Column::Bundle, Column::Id};
        case GroupMode::Format: return {Column::Name, Column::Vendor, Column::Bundle, Column::Id};
        case GroupMode::Developer: return {Column::Name, Column::Format, Column::Bundle, Column::Id};
        case GroupMode::Bundle: return {Column::Name, Column::Format, Column::Vendor, Column::Id};
    }
    return {};
}

void PluginList::setOnPluginSelected(std::function<void(const std::string& format, const std::string& pluginId, const std::string& name)> callback) {
    onPluginSelected_ = callback;
}

PluginList::Selection PluginList::getSelection() const {
    Selection sel;
    sel.format = selectedPluginFormat_;
    sel.pluginId = selectedPluginId_;
    sel.hasSelection = !selectedPluginFormat_.empty() && !selectedPluginId_.empty();
    return sel;
}

void PluginList::clearSelection() {
    selectedPluginFormat_.clear();
    selectedPluginId_.clear();
}

void PluginList::setSearchFilter(const char* filter) {
    if (filter) {
        strncpy(searchFilter_, filter, sizeof(searchFilter_) - 1);
        searchFilter_[sizeof(searchFilter_) - 1] = '\0';
        searchCollapsedGroups_.clear();
        rowsDirty_ = true;
    }
}

const char* PluginList::getSearchFilter() const {
    return searchFilter_;
}

void PluginList::groupMode(GroupMode mode) {
    if (mode == groupMode_)
        return;
    groupMode_ = mode;
    searchCollapsedGroups_.clear();
    rowsDirty_ = true;
}

void PluginList::expandAllGroups() {
    if (searchActive())
        searchCollapsedGroups_.clear();
    else
        for (const auto& group : groups_)
            expandedGroups_[static_cast<size_t>(groupMode_)].insert(group.key);
    rowsDirty_ = true;
}

void PluginList::collapseAllGroups() {
    if (searchActive())
        for (const auto& group : groups_)
            searchCollapsedGroups_.insert(group.key);
    else
        expandedGroups_[static_cast<size_t>(groupMode_)].clear();
    rowsDirty_ = true;
}

// While searching, matching groups start expanded and collapse state is kept separately
bool PluginList::isGroupExpanded(const std::string& key) const {
    if (searchActive())
        return !searchCollapsedGroups_.contains(key);
    return expandedGroups_[static_cast<size_t>(groupMode_)].contains(key);
}

void PluginList::setGroupExpanded(const std::string& key, bool expanded) {
    if (searchActive()) {
        if (expanded)
            searchCollapsedGroups_.erase(key);
        else
            searchCollapsedGroups_.insert(key);
        return;
    }
    auto& set = expandedGroups_[static_cast<size_t>(groupMode_)];
    if (expanded)
        set.insert(key);
    else
        set.erase(key);
}

void PluginList::rebuildRows() {
    rowsDirty_ = false;
    rows_.clear();
    groups_.clear();

    auto indices = filterIndices();
    if (groupMode_ == GroupMode::None) {
        sortFlat(indices);
        rows_.reserve(indices.size());
        for (int idx : indices)
            rows_.push_back({false, idx});
        return;
    }

    buildGroups(indices);
    for (size_t g = 0; g < groups_.size(); ++g) {
        rows_.push_back({true, static_cast<int>(g)});
        if (isGroupExpanded(groups_[g].key))
            for (int idx : groups_[g].members)
                rows_.push_back({false, idx});
    }
}

std::vector<int> PluginList::filterIndices() const {
    auto filter = remidy_imgui::PluginFiltering::toLower(searchFilter_);

    std::vector<int> indices;
    indices.reserve(availablePlugins_.size());
    for (size_t i = 0; i < availablePlugins_.size(); ++i) {
        if (!filter.empty() &&
            lowerNames_[i].find(filter) == std::string::npos &&
            lowerVendors_[i].find(filter) == std::string::npos)
            continue;
        indices.push_back(static_cast<int>(i));
    }
    return indices;
}

void PluginList::sortFlat(std::vector<int>& indices) const {
    if (flatSortSpecs_.empty())
        return;

    auto columns = activeColumns();
    auto cmp = [&](int lhsIdx, int rhsIdx) {
        const auto& a = availablePlugins_[static_cast<size_t>(lhsIdx)];
        const auto& b = availablePlugins_[static_cast<size_t>(rhsIdx)];
        for (const auto& s : flatSortSpecs_) {
            if (s.column < 0 || s.column >= static_cast<int>(columns.size()))
                continue;
            int delta = 0;
            switch (columns[static_cast<size_t>(s.column)]) {
                case Column::Format: delta = compareCI(a.format, b.format); break;
                case Column::Name: delta = compareCI(a.name, b.name); break;
                case Column::Vendor: delta = compareCI(a.vendor, b.vendor); break;
                case Column::Id: delta = compareCI(a.id, b.id); break;
                case Column::Bundle:
                    delta = compareCI(bundleLabels_[static_cast<size_t>(lhsIdx)], bundleLabels_[static_cast<size_t>(rhsIdx)]);
                    break;
            }
            if (delta != 0)
                return (s.direction == ImGuiSortDirection_Ascending) ? (delta < 0) : (delta > 0);
        }
        // Tiebreaker to get deterministic order
        if (int t = compareCI(a.name, b.name); t != 0) return t < 0;
        if (int t = compareCI(a.vendor, b.vendor); t != 0) return t < 0;
        if (int t = compareCI(a.id, b.id); t != 0) return t < 0;
        return compareCI(a.format, b.format) < 0;
    };
    std::sort(indices.begin(), indices.end(), cmp);
}

void PluginList::buildGroups(const std::vector<int>& indices) {
    std::unordered_map<std::string, size_t> groupByKey;
    for (int idx : indices) {
        const auto& p = availablePlugins_[static_cast<size_t>(idx)];
        Group candidate;
        switch (groupMode_) {
            case GroupMode::Format:
                candidate.key = p.format;
                candidate.label = p.format;
                break;
            case GroupMode::Developer: {
                auto vendor = trim(p.vendor);
                candidate.key = remidy_imgui::PluginFiltering::toLower(vendor);
                candidate.label = vendor;
                break;
            }
            case GroupMode::Bundle:
                candidate.key = p.bundle;
                candidate.label = bundleLabels_[static_cast<size_t>(idx)];
                if (candidate.label != p.bundle)
                    candidate.tooltip = p.bundle;
                break;
            case GroupMode::None:
                break;
        }
        if (candidate.key.empty()) {
            candidate.placeholder = true;
            candidate.label = groupMode_ == GroupMode::Developer ? "(Unknown developer)"
                            : groupMode_ == GroupMode::Bundle ? "(No bundle)"
                            : "(Unknown)";
        }

        auto [it, inserted] = groupByKey.try_emplace(candidate.key, groups_.size());
        if (inserted)
            groups_.push_back(std::move(candidate));
        groups_[it->second].members.push_back(idx);
    }

    bool descending = groupSortDescending_;
    auto byName = [&](int lhsIdx, int rhsIdx) {
        const auto& a = availablePlugins_[static_cast<size_t>(lhsIdx)];
        const auto& b = availablePlugins_[static_cast<size_t>(rhsIdx)];
        if (int t = compareCI(a.name, b.name); t != 0) return descending ? t > 0 : t < 0;
        if (int t = compareCI(a.format, b.format); t != 0) return t < 0;
        return compareCI(a.id, b.id) < 0;
    };
    for (auto& group : groups_)
        std::sort(group.members.begin(), group.members.end(), byName);

    // Placeholder groups always go last regardless of direction
    std::sort(groups_.begin(), groups_.end(), [&](const Group& a, const Group& b) {
        if (a.placeholder != b.placeholder)
            return b.placeholder;
        int t = compareCI(a.label, b.label);
        return descending ? t > 0 : t < 0;
    });
}

}
