#include "PluginSelectorDialog.hpp"
#include "../AppModel.hpp"
#include <imgui.h>
#include <iostream>
#include <algorithm>

namespace uapmd::gui {

PluginSelectorDialog::PluginSelectorDialog() {
    refreshPluginList();
}

PluginSelectorDialog::~PluginSelectorDialog() {
}

void PluginSelectorDialog::show(std::function<void(const std::string& format, const std::string& id)> callback) {
    if (isOpen_) return;

    onPluginSelected_ = callback;
    isOpen_ = true;
}

void PluginSelectorDialog::hide() {
    isOpen_ = false;
    ImGui::CloseCurrentPopup();
}


void PluginSelectorDialog::render() {
    // Open popup when requested
    if (isOpen_ && !ImGui::IsPopupOpen("Select a Plugin")) {
        ImGui::OpenPopup("Select a Plugin");
    }

    // Set modal popup size
    ImGui::SetNextWindowSize(ImVec2(900, 700), ImGuiCond_Always);

    if (ImGui::BeginPopupModal("Select a Plugin", &isOpen_, ImGuiWindowFlags_NoResize)) {
        // Control buttons
        if (ImGui::Button("Refresh Plugins")) {
            refreshPluginList();
        }
        ImGui::SameLine();
        if (ImGui::Button("Rescan Plugins")) {
            rescanPlugins();
        }
        ImGui::SameLine();
        ImGui::Checkbox("Show Deny List", &showDenyList_);

        // Search filter
        ImGui::InputText("Search", searchFilter_, sizeof(searchFilter_));

        ImGui::Separator();

        // Plugin list (with fixed height to leave room for buttons)
        auto filteredPlugins = getFilteredPlugins();
        const auto& pluginList = showDenyList_ ? denyListPlugins_ : filteredPlugins;

        float windowHeight = ImGui::GetContentRegionAvail().y;
        float buttonAreaHeight = 50.0f; // Reserve space for buttons at bottom
        float tableHeight = windowHeight - buttonAreaHeight;

        if (ImGui::BeginTable("PluginTable", 4, ImGuiTableFlags_Resizable | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY, ImVec2(0, tableHeight))) {
            ImGui::TableSetupColumn("Format", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Vendor", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            for (size_t i = 0; i < pluginList.size(); i++) {
                const auto& plugin = pluginList[i];
                bool isSelected = (selectedPluginFormat_ == plugin.format && selectedPluginId_ == plugin.id);

                ImGui::TableNextRow();

                // Make the entire row selectable
                ImGui::TableNextColumn();
                std::string selectableId = plugin.format + "##" + plugin.id + "##" + std::to_string(i);
                if (ImGui::Selectable(selectableId.c_str(), isSelected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowItemOverlap)) {
                    selectedPluginFormat_ = plugin.format;
                    selectedPluginId_ = plugin.id;
                    std::cout << "Selected plugin: " << plugin.format << " - " << plugin.name << " (ID: " << plugin.id << ")" << std::endl;
                }

                // Draw the actual content on top of the selectable
                ImGui::SameLine();
                ImGui::Text("%s", plugin.format.c_str());

                // Name column
                ImGui::TableNextColumn();
                ImGui::Text("%s", plugin.name.c_str());

                // Vendor column
                ImGui::TableNextColumn();
                ImGui::Text("%s", plugin.vendor.c_str());

                // ID column
                ImGui::TableNextColumn();
                ImGui::Text("%s", plugin.id.c_str());
            }
            ImGui::EndTable();
        }

        ImGui::Separator();

        // Instantiate and Cancel buttons
        bool canInstantiate = !selectedPluginFormat_.empty() && !selectedPluginId_.empty() && !showDenyList_;
        if (!canInstantiate) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("Instantiate Plugin")) {
            instantiateSelectedPlugin();
            hide();
        }
        if (!canInstantiate) {
            ImGui::EndDisabled();
        }

        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            hide();
        }

        ImGui::EndPopup();
    }

    // Check if popup was closed by clicking outside or ESC
    if (!ImGui::IsPopupOpen("Select a Plugin") && isOpen_) {
        isOpen_ = false;
    }
}

void PluginSelectorDialog::refreshPluginList() {
    availablePlugins_.clear();
    denyListPlugins_.clear();

    auto& catalog = uapmd::AppModel::instance().sequencer().catalog();

    // Get available plugins
    auto plugins = catalog.getPlugins();
    std::cout << "Total plugins found: " << plugins.size() << std::endl;
    for (auto* plugin : plugins) {
        std::string format = plugin->format();
        std::string id = plugin->pluginId();
        std::string name = plugin->displayName();
        std::string vendor = plugin->vendorName();

        availablePlugins_.push_back({
            .format = format,
            .id = id,
            .name = name,
            .vendor = vendor
        });

        // Debug logging for Vital specifically
        if (name.find("Vital") != std::string::npos) {
            std::cout << "Found Vital plugin: " << format << " - " << name << " (ID: " << id << ")" << std::endl;
        }
    }

    // Get deny list
    auto denyList = catalog.getDenyList();
    for (auto* plugin : denyList) {
        denyListPlugins_.push_back({
            .format = plugin->format(),
            .id = plugin->pluginId(),
            .name = plugin->displayName(),
            .vendor = plugin->vendorName()
        });
    }

    // Check if the currently selected plugin still exists in the refreshed list
    bool selectedPluginExists = false;
    if (!selectedPluginFormat_.empty() && !selectedPluginId_.empty()) {
        for (const auto& plugin : availablePlugins_) {
            if (plugin.format == selectedPluginFormat_ && plugin.id == selectedPluginId_) {
                selectedPluginExists = true;
                break;
            }
        }
    }

    // Clear selection if the plugin no longer exists
    if (!selectedPluginExists) {
        selectedPluginFormat_.clear();
        selectedPluginId_.clear();
    }
}

void PluginSelectorDialog::instantiateSelectedPlugin() {
    if (selectedPluginFormat_.empty() || selectedPluginId_.empty()) {
        return;
    }

    if (onPluginSelected_) {
        onPluginSelected_(selectedPluginFormat_, selectedPluginId_);
    }
}

void PluginSelectorDialog::rescanPlugins() {
    auto& sequencer = uapmd::AppModel::instance().sequencer();
    sequencer.performPluginScanning(true); // Force rescan
    refreshPluginList();
}

std::vector<PluginEntry> PluginSelectorDialog::getFilteredPlugins() const {
    if (strlen(searchFilter_) == 0) {
        return availablePlugins_;
    }

    std::vector<PluginEntry> filtered;
    std::string filter = searchFilter_;
    std::transform(filter.begin(), filter.end(), filter.begin(), ::tolower);

    for (const auto& plugin : availablePlugins_) {
        std::string name = plugin.name;
        std::string vendor = plugin.vendor;
        std::string format = plugin.format;

        std::transform(name.begin(), name.end(), name.begin(), ::tolower);
        std::transform(vendor.begin(), vendor.end(), vendor.begin(), ::tolower);
        std::transform(format.begin(), format.end(), format.begin(), ::tolower);

        if (name.find(filter) != std::string::npos ||
            vendor.find(filter) != std::string::npos ||
            format.find(filter) != std::string::npos) {
            filtered.push_back(plugin);
        }
    }

    return filtered;
}

}