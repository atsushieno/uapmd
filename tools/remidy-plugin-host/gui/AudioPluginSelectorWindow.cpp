#include "AudioPluginSelectorWindow.hpp"
#include "../AppModel.hpp"
#include <imgui.h>
#include <algorithm>
#include <iostream>
#include <ranges>

namespace uapmd::gui {

AudioPluginSelectorWindow::AudioPluginSelectorWindow() {
    refreshPluginList();
}

void AudioPluginSelectorWindow::render() {
    if (!ImGui::Begin("Audio Plugin Selector", &isOpen_)) {
        ImGui::End();
        return;
    }

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

    // Plugin list
    auto filteredPlugins = getFilteredPlugins();
    const auto& pluginList = showDenyList_ ? denyListPlugins_ : filteredPlugins;

    if (ImGui::BeginTable("PluginTable", 4, ImGuiTableFlags_Resizable | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("Format", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Vendor", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (size_t i = 0; i < pluginList.size(); i++) {
            const auto& plugin = pluginList[i];

            ImGui::TableNextRow();

            // Format column
            ImGui::TableNextColumn();
            ImGui::Text("%s", plugin.format.c_str());

            // Name column - make it selectable
            ImGui::TableNextColumn();
            bool isSelected = (selectedPlugin_ == static_cast<int>(i));
            if (ImGui::Selectable(plugin.name.c_str(), isSelected, ImGuiSelectableFlags_SpanAllColumns)) {
                selectedPlugin_ = static_cast<int>(i);
            }

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

    // Instantiate button
    bool canInstantiate = selectedPlugin_ >= 0 && selectedPlugin_ < static_cast<int>(pluginList.size()) && !showDenyList_;
    if (!canInstantiate) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Instantiate Plugin")) {
        instantiateSelectedPlugin();
    }
    if (!canInstantiate) {
        ImGui::EndDisabled();
    }

    ImGui::End();
}

void AudioPluginSelectorWindow::refreshPluginList() {
    availablePlugins_.clear();
    denyListPlugins_.clear();

    // Use real plugin catalog (same as WebView version)
    auto& catalog = uapmd::AppModel::instance().sequencer().catalog();

    // Get available plugins
    auto plugins = catalog.getPlugins();
    for (auto* plugin : plugins) {
        availablePlugins_.push_back({
            .format = plugin->format(),
            .id = plugin->pluginId(),
            .name = plugin->displayName(),
            .vendor = plugin->vendorName()
        });
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

    // Reset selection if it's out of bounds
    if (selectedPlugin_ >= static_cast<int>(availablePlugins_.size())) {
        selectedPlugin_ = -1;
    }
}

void AudioPluginSelectorWindow::instantiateSelectedPlugin() {
    if (selectedPlugin_ < 0 || selectedPlugin_ >= static_cast<int>(availablePlugins_.size())) {
        return;
    }

    const auto& plugin = availablePlugins_[selectedPlugin_];

    // Generate a unique instancing ID
    static int instancingIdCounter = 1;
    int instancingId = instancingIdCounter++;

    uapmd::AppModel::instance().instantiatePlugin(instancingId, plugin.format, plugin.id);

    std::cout << "Instantiating plugin: " << plugin.name << " (" << plugin.format
              << ") with ID " << instancingId << std::endl;
}

void AudioPluginSelectorWindow::rescanPlugins() {
    // Use real plugin scanning (same as WebView version)
    auto& sequencer = uapmd::AppModel::instance().sequencer();
    sequencer.performPluginScanning(true); // Force rescan
    refreshPluginList();
}

std::vector<PluginEntry> AudioPluginSelectorWindow::getFilteredPlugins() const {
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