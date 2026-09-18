#include "JsfxResourcesWindow.hpp"

#if UAPMD_HAS_JSFX

#include <algorithm>
#include <filesystem>
#include <memory>

#include <uapmd-app-model/uapmd-app-model.hpp>

#include "imgui.h"

namespace uapmd_app {

    namespace {
        uapmd_jsfx::JsfxPluginFormat* jsfxFormat() {
            return AppModel::instance().jsfxPluginFormat();
        }

        // How many JSFX effects the catalog holds, which is the only number that says
        // whether any of this worked.
        size_t jsfxEffectCount() {
            size_t count = 0;
            for (auto* entry : AppModel::instance().pluginScanTool().catalog().getPlugins())
                if (entry && entry->format() == "JSFX")
                    count++;
            return count;
        }

        // Splits a document's folder-relative name into the folder it belongs in and the
        // name of the file itself, so that copying a folder keeps its shape.
        std::pair<std::string, std::string> splitRelativeName(const std::string& relative) {
            std::filesystem::path path{relative};
            return {path.parent_path().generic_string(), path.filename().string()};
        }
    }

    void JsfxResourcesWindow::report(std::string message, bool isError) {
        status_ = std::move(message);
        status_is_error_ = isError;
    }

    // Effects only reach the plugin list through a scan, so everything here ends with
    // one. Removing a folder has to drop its effects as much as adding one has to find
    // them, which is why this is a refresh rather than a merge.
    //
    // The message the caller left on screen says something is happening, so this
    // replaces it when it stops -- otherwise it says so for ever.
    void JsfxResourcesWindow::refreshCatalog() {
        busy_ = true;
        AppModel::instance().refreshFastScannedPlugins([this](std::string syncError) {
            busy_ = false;
            registered_folders_ = uapmd_jsfx::jsfxRegisteredFolders();
            const auto available = std::to_string(jsfxEffectCount()) + " JSFX effects available.";
            if (syncError.empty())
                report(available, false);
            else
                // Still says how many there are: some folders may have been read even
                // though one of them could not be.
                report(syncError + " -- " + available, true);
        });
    }

    std::string JsfxResourcesWindow::destinationName() const {
        return std::string{destination_.data()};
    }

    void JsfxResourcesWindow::loadFromFormat() {
        paths_loaded_ = true;
        registered_folders_ = uapmd_jsfx::jsfxRegisteredFolders();
        auto* jsfx = jsfxFormat();
        if (!jsfx)
            return;
        search_paths_ = jsfx->searchPaths();
        use_defaults_ = jsfx->useDefaultSearchPaths();
    }

    void JsfxResourcesWindow::registerPickedFolder(const uapmd::FolderPickResult& picked) {
        auto name = uapmd_jsfx::registerJsfxFolder(picked.token, picked.display_name);
        registered_folders_ = uapmd_jsfx::jsfxRegisteredFolders();
        if (name.empty()) {
            pick_in_flight_ = false;
            report("That folder could not be registered.", true);
            return;
        }
        report("Registered " + name + ". Reading it...", false);
        syncRegisteredFolders();
    }

    void JsfxResourcesWindow::syncRegisteredFolders() {
        // Refreshing the catalog re-reads the registered folders on its own, so this
        // asks for that rather than reading them and then asking, which would walk
        // every folder twice.
        pick_in_flight_ = false;
        report("Reading the registered folders...", false);
        refreshCatalog();
    }

    const char* JsfxResourcesWindow::folderButtonLabel(bool foldersArePlaces) {
        // On the platforms where a folder is a grant rather than a place, its content is
        // copied here and re-copied whenever it changes. Calling that "adding a folder to
        // search" would describe something the platform cannot do.
        return foldersArePlaces ? "Add a folder to search..." : "Import JSFX from a folder...";
    }

    void JsfxResourcesWindow::applyToFormat() {
        auto* jsfx = jsfxFormat();
        if (!jsfx)
            return;
        jsfx->searchPaths(search_paths_, use_defaults_);
        AppModel::instance().pluginScanTool().saveSearchPathSettings();
        refreshCatalog();
    }

    void JsfxResourcesWindow::finishImport(uint32_t filesWritten,
                                           const std::vector<std::string>& failures) {
        pick_in_flight_ = false;
        if (failures.empty())
            report("Copied in " + std::to_string(filesWritten)
                   + " file(s). Looking for new effects...", false);
        else {
            std::string message = failures.front();
            if (failures.size() > 1)
                message += " (and " + std::to_string(failures.size() - 1) + " more)";
            report(message, true);
        }
        if (filesWritten > 0)
            refreshCatalog();
    }

    void JsfxResourcesWindow::beginFileImport() {
        auto* provider = AppModel::instance().documentProvider();
        if (!provider) {
            report("There is no file picker on this platform.", true);
            return;
        }

        std::vector<uapmd::DocumentFilter> filters;
        filters.push_back({"JSFX effects and archives",
                           {"application/zip", "text/plain", "application/octet-stream"},
                           {"*.jsfx", "*.jsfx-inc", "*.zip"}});
        filters.push_back({"All files", {"*/*"}, {"*"}});

        // Read now rather than in the callback: picking is asynchronous and the field is
        // free to change while the picker is up.
        const std::string destination = destinationName();

        pick_in_flight_ = true;
        report("Choosing files...", false);

        provider->pickOpenDocuments(filters, true,
            [this, destination](uapmd::DocumentPickResult picked) {
                if (!picked.success) {
                    pick_in_flight_ = false;
                    report("Could not open the file picker: " + picked.error, true);
                    return;
                }
                if (picked.handles.empty()) {
                    pick_in_flight_ = false;
                    report("Nothing was chosen.", false);
                    return;
                }
                importDocuments(std::move(picked.handles), destination);
            });
    }

    void JsfxResourcesWindow::beginFolderPick() {
        auto* provider = AppModel::instance().documentProvider();
        if (!provider) {
            report("There is no folder picker on this platform.", true);
            return;
        }

        const std::string destination = destinationName();

        pick_in_flight_ = true;
        report("Choosing a folder...", false);

        provider->pickFolder([this, destination](uapmd::FolderPickResult picked) {
            if (!picked.success) {
                pick_in_flight_ = false;
                report("Could not open the folder picker: " + picked.error, true);
                return;
            }
            if (picked.path.empty() && picked.documents.empty()) {
                pick_in_flight_ = false;
                report("Nothing was chosen.", false);
                return;
            }

            // A folder that is a location goes straight into the search paths and is
            // read where it sits; nothing is copied and nothing goes stale.
            if (!picked.path.empty()) {
                pick_in_flight_ = false;
                const auto chosen = picked.path.string();
                if (std::find(search_paths_.begin(), search_paths_.end(), chosen)
                        != search_paths_.end()) {
                    report("That folder is already being looked in.", true);
                    return;
                }
                search_paths_.emplace_back(chosen);
                applyToFormat();
                report("Added " + chosen + ". Looking again for effects...", false);
                return;
            }

            // A folder that is a grant is registered instead: the grant is kept, and the
            // folder is re-read whenever effects are looked for again. Files the user
            // adds to it later therefore show up, which is the point.
            if (!picked.token.empty()) {
                registerPickedFolder(picked);
                return;
            }

            // And a platform that gives neither -- a browser -- cannot keep a folder at
            // all, so the files it handed over are copied in instead. It is the same
            // copy the archive import does, under the folder's own name.
            report("This platform cannot keep a folder; copying its effects in.", false);
            importDocuments(std::move(picked.documents),
                            destination.empty() ? picked.display_name : destination);
        });
    }

    void JsfxResourcesWindow::importDocuments(std::vector<uapmd::DocumentHandle> documents,
                                              std::string subdirectory) {
        auto* provider = AppModel::instance().documentProvider();
        if (!provider) {
            pick_in_flight_ = false;
            report("There is nothing to read documents with on this platform.", true);
            return;
        }

        // Each document is read on its own, so the outcome is reported by whichever read
        // finishes last rather than by any particular one.
        auto remaining = std::make_shared<size_t>(documents.size());
        auto written = std::make_shared<uint32_t>(0);
        auto failures = std::make_shared<std::vector<std::string>>();

        for (auto& handle : documents) {
            // The display name is all a picked document reliably carries, and for one
            // that came out of a folder it is the path it had inside that folder.
            const std::string relative = handle.display_name.empty() ? handle.id
                                                                     : handle.display_name;
            auto [parent, leaf] = splitRelativeName(relative);
            std::string destination = subdirectory;
            if (!parent.empty())
                destination = destination.empty() ? parent : destination + "/" + parent;

            provider->readDocument(handle,
                [this, leaf, destination, remaining, written, failures](
                        uapmd::DocumentIOResult io, std::vector<uint8_t> data) {
                    if (!io.success)
                        failures->emplace_back(leaf + ": " + io.error);
                    else {
                        auto result = uapmd_jsfx::importJsfxContent(
                                leaf, data.data(), data.size(), destination);
                        if (result.success)
                            *written += result.filesWritten;
                        else
                            failures->emplace_back(leaf + ": " + result.error);
                    }

                    if (--*remaining > 0)
                        return;
                    finishImport(*written, *failures);
                });
        }
    }

    void JsfxResourcesWindow::render() {
        if (!open_)
            return;
        if (!paths_loaded_)
            loadFromFormat();

        ImGui::SetNextWindowSize(ImVec2(660, 460), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("JSFX Effects###jsfx-resources", &open_)) {
            ImGui::End();
            return;
        }

        auto* jsfx = jsfxFormat();
        if (!jsfx) {
            ImGui::TextUnformatted("JSFX is unavailable in this build.");
            ImGui::End();
            return;
        }

        auto* provider = AppModel::instance().documentProvider();
        const bool foldersArePlaces = provider && provider->folderPathsAreUsable();
        const auto contentDir = uapmd_jsfx::jsfxUserContentDirectory();
        const bool canCopy = provider && !contentDir.empty();

        ImGui::SeparatorText("Copy effects in");

        ImGui::SetNextItemWidth(220 * ImGui::GetFontSize() / 13.0f);
        ImGui::InputTextWithHint("Copy into", "a name for this set of effects",
                                 destination_.data(), destination_.size());
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("A folder of your own to keep these files in. Leave it empty\n"
                              "to copy them in as they are.");

        ImGui::BeginDisabled(pick_in_flight_ || busy_ || !canCopy);
        if (ImGui::Button("Copy in files or a .zip..."))
            beginFileImport();
        ImGui::EndDisabled();

        if (contentDir.empty())
            ImGui::TextUnformatted("There is nowhere writable to copy effects to on this platform.");
        else {
            ImGui::TextUnformatted("Copies are kept in:");
            ImGui::TextWrapped("%s", contentDir.string().c_str());
        }

        ImGui::SeparatorText("Where effects are looked for");

        ImGui::BeginDisabled(pick_in_flight_ || busy_ || !provider);
        if (ImGui::Button(folderButtonLabel(foldersArePlaces)))
            beginFolderPick();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(foldersArePlaces
                    ? "Choose a folder of effects. It stays where it is and is read\n"
                      "where it sits."
                    : "Choose a folder of effects. This platform hands over a folder's\n"
                      "files rather than the folder itself, so the folder is remembered\n"
                      "and re-read: effects you add to it later still turn up."
#if defined(__ANDROID__)
                      "\n\nAndroid refuses to hand over Download, Android/data, or the\n"
                      "whole of internal storage. Keep effects in a folder of their own,\n"
                      "such as a top-level JSFX folder."
#endif
                    );

        if (!registered_folders_.empty()) {
            ImGui::SameLine();
            ImGui::BeginDisabled(pick_in_flight_ || busy_);
            if (ImGui::Button("Re-read registered folders"))
                syncRegisteredFolders();
            ImGui::EndDisabled();
        }

        std::string unregisterToken;
        for (auto& folder : registered_folders_) {
            ImGui::PushID(folder.token.c_str());
            ImGui::Bullet();
            ImGui::TextUnformatted(folder.name.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("(registered)");
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove"))
                unregisterToken = folder.token;
            ImGui::PopID();
        }
        if (!unregisterToken.empty()) {
            // The grant goes back as well as the record of it: forgetting the token
            // alone would leave the platform holding a permission nothing uses.
            if (provider)
                provider->releaseFolder(unregisterToken);
            uapmd_jsfx::unregisterJsfxFolder(unregisterToken);
            registered_folders_ = uapmd_jsfx::jsfxRegisteredFolders();
            refreshCatalog();
            report("Removed a registered folder. Looking again for effects...", false);
        }

        if (ImGui::Checkbox("Also look in this platform's usual locations", &use_defaults_))
            applyToFormat();

        const auto defaults = jsfx->defaultSearchPaths();
        if (use_defaults_) {
            if (defaults.empty())
                ImGui::TextDisabled("This platform has no conventional JSFX location.");
            else
                for (auto& path : defaults)
                    ImGui::BulletText("%s", path.string().c_str());
        }

        int removeIndex = -1;
        for (size_t i = 0; i < search_paths_.size(); i++) {
            ImGui::PushID(static_cast<int>(i));
            ImGui::Bullet();
            ImGui::TextUnformatted(search_paths_[i].c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove"))
                removeIndex = static_cast<int>(i);
            ImGui::PopID();
        }
        if (removeIndex >= 0) {
            search_paths_.erase(search_paths_.begin() + removeIndex);
            applyToFormat();
            report("Removed a folder. Looking again for effects...", false);
        }

        if (!status_.empty()) {
            ImGui::Separator();
            if (status_is_error_)
                ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", status_.c_str());
            else
                ImGui::TextWrapped("%s", status_.c_str());
        }

        ImGui::End();
    }

}

#endif
