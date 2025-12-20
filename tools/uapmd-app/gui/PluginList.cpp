#include "PluginList.hpp"
#include <algorithm>
#include <iostream>

namespace uapmd::gui {

PluginList::PluginList() {
    std::fill(std::begin(searchFilter_), std::end(searchFilter_), '\0');
}

void PluginList::setPlugins(const std::vector<PluginEntry>& plugins) {
    availablePlugins_ = plugins;
}

void PluginList::render() {
    ImGui::InputText("Search", searchFilter_, sizeof(searchFilter_));

    if (ImGui::BeginTable("PluginTable", 4,
                          ImGuiTableFlags_Resizable | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_Sortable,
                          ImVec2(0, 300))) {
        ImGui::TableSetupColumn("Format", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Vendor", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        std::vector<int> indices = filterAndBuildIndices();
        sortIndices(indices);

        for (int sortedIndex : indices) {
            const auto& plugin = availablePlugins_[static_cast<size_t>(sortedIndex)];

            bool isSelected = (selectedPluginFormat_ == plugin.format && selectedPluginId_ == plugin.id);

            ImGui::TableNextRow();
            ImGui::TableNextColumn();

            std::string selectableId = "##" + plugin.id + "##" + std::to_string(sortedIndex);
            if (ImGui::Selectable(selectableId.c_str(), isSelected, ImGuiSelectableFlags_SpanAllColumns)) {
                selectedPluginFormat_ = plugin.format;
                selectedPluginId_ = plugin.id;
                std::cout << "[GUI] Selected plugin: format='" << plugin.format << "', id='" << plugin.id
                          << "', name='" << plugin.name << "'" << std::endl;

                if (onPluginSelected_) {
                    onPluginSelected_(plugin.format, plugin.id, plugin.name);
                }
            }

            ImGui::SameLine();
            ImGui::Text("%s", plugin.format.c_str());

            ImGui::TableNextColumn();
            ImGui::Text("%s", plugin.name.c_str());

            ImGui::TableNextColumn();
            ImGui::Text("%s", plugin.vendor.c_str());

            ImGui::TableNextColumn();
            ImGui::Text("%s", plugin.id.c_str());
        }
        ImGui::EndTable();
    }
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
    }
}

const char* PluginList::getSearchFilter() const {
    return searchFilter_;
}

std::vector<int> PluginList::filterAndBuildIndices() {
    std::string filter = searchFilter_;
    std::transform(filter.begin(), filter.end(), filter.begin(), ::tolower);

    std::vector<int> indices;
    indices.reserve(availablePlugins_.size());

    for (size_t i = 0; i < availablePlugins_.size(); ++i) {
        const auto& p = availablePlugins_[i];
        if (!filter.empty()) {
            std::string name = p.name;
            std::string vendor = p.vendor;
            std::transform(name.begin(), name.end(), name.begin(), ::tolower);
            std::transform(vendor.begin(), vendor.end(), vendor.begin(), ::tolower);
            if (name.find(filter) == std::string::npos && vendor.find(filter) == std::string::npos) {
                continue;
            }
        }
        indices.push_back(static_cast<int>(i));
    }

    return indices;
}

void PluginList::sortIndices(std::vector<int>& indices) {
    if (indices.empty()) return;

    if (ImGuiTableSortSpecs* sort_specs = ImGui::TableGetSortSpecs()) {
        if (sort_specs->SpecsCount > 0) {
            auto cmp = [&](int lhsIdx, int rhsIdx) {
                const auto& a = availablePlugins_[static_cast<size_t>(lhsIdx)];
                const auto& b = availablePlugins_[static_cast<size_t>(rhsIdx)];
                for (int n = 0; n < sort_specs->SpecsCount; n++) {
                    const ImGuiTableColumnSortSpecs* s = &sort_specs->Specs[n];
                    int delta = 0;
                    switch (s->ColumnIndex) {
                        case 0: delta = a.format.compare(b.format); break;
                        case 1: delta = a.name.compare(b.name); break;
                        case 2: delta = a.vendor.compare(b.vendor); break;
                        case 3: delta = a.id.compare(b.id); break;
                        default: break;
                    }
                    if (delta != 0) {
                        return (s->SortDirection == ImGuiSortDirection_Ascending) ? (delta < 0) : (delta > 0);
                    }
                }
                // Tiebreaker to get deterministic order
                if (int t = a.name.compare(b.name); t != 0) return t < 0;
                if (int t = a.vendor.compare(b.vendor); t != 0) return t < 0;
                if (int t = a.id.compare(b.id); t != 0) return t < 0;
                return a.format < b.format;
            };
            std::sort(indices.begin(), indices.end(), cmp);
        }
    }
}

}
