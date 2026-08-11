#include "AddinManagerWindow.hpp"

#include <imgui.h>

namespace uapmd_app_gui {

AddinManagerWindow::AddinManagerWindow(uapmd::AddinManager& runtime)
    : runtime_(runtime) {}

void AddinManagerWindow::show() {
    open_ = true;
}

void AddinManagerWindow::hide() {
    open_ = false;
}

bool AddinManagerWindow::isOpen() const noexcept {
    return open_;
}

void AddinManagerWindow::render(float uiScale) {
    if (!open_)
        return;

    ImGui::SetNextWindowSize(ImVec2(760.0f * uiScale, 430.0f * uiScale), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Addin Manager", &open_)) {
        ImGui::End();
        return;
    }

    if (!uapmd::AddinManager::supportsDynamicLoading()) {
        ImGui::TextWrapped("This WebAssembly build uses compiled-in addins. Changes to enabled addins take effect when the application restarts.");
        ImGui::Separator();
    } else {
        ImGui::TextWrapped("Install addin libraries in one of these directories:");
        const auto& directories = runtime_.addinDirectories();
        if (directories.empty())
            ImGui::TextUnformatted("No addin directory is available on this platform.");
        for (const auto& directory : directories)
            ImGui::TextUnformatted(directory.string().c_str());
        ImGui::Separator();
    }

    if (!runtime_.lastError().empty()) {
        ImGui::TextWrapped("Last error: %s", runtime_.lastError().c_str());
        ImGui::Separator();
    }

    if (ImGui::BeginTable("Addins", 6,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
            ImVec2(0.0f, -1.0f))) {
        ImGui::TableSetupColumn("Enabled", ImGuiTableColumnFlags_WidthFixed, 72.0f * uiScale);
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("Package / Addin");
        ImGui::TableSetupColumn("Path");
        ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 92.0f * uiScale);
        ImGui::TableSetupColumn("Library");
        ImGui::TableHeadersRow();

        const auto addins = runtime_.addins();
        for (const auto& addin : addins) {
            ImGui::PushID((addin.package_id + "/" + addin.addin_id).c_str());
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            bool enabled = addin.state == uapmd::AddinState::Active;
            if (ImGui::Checkbox("##enabled", &enabled))
                runtime_.setEnabled(addin.package_id, addin.addin_id, enabled);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(addin.name.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%s / %s", addin.package_id.c_str(), addin.addin_id.c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(addin.path.c_str());
            ImGui::TableSetColumnIndex(4);
            ImGui::TextUnformatted(uapmd::addinStateName(addin.state));
            ImGui::TableSetColumnIndex(5);
            if (addin.built_in)
                ImGui::TextUnformatted("Built-in");
            else
                ImGui::TextUnformatted(addin.library_path.filename().string().c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

} // namespace uapmd_app_gui
