#pragma once

#if UAPMD_HAS_JSFX

#include <array>
#include <string>
#include <vector>

#include <uapmd-file/IDocumentProvider.hpp>
#include <uapmd-format-jsfx/uapmd-format-jsfx.hpp>

namespace uapmd_app {

    // Where the user manages the JSFX effects available to them.
    //
    // JSFX is a filesystem format, so this window is about locations and files rather
    // than settings: which folders are searched, and getting content into a folder that
    // is. Both matter most on the platforms with no REAPER installation to inherit from,
    // where adding something by hand is the only way anything gets in.
    //
    // A folder is handed over one of two ways, and the platform decides which: desktop
    // gives a location, which is added to the search paths and read where it sits;
    // Android and iOS give a grant, which is registered and copied in. Either way the
    // user picks a folder and its effects turn up, so there is one button for it --
    // IDocumentProvider::folderPathsAreUsable() only decides what to call it.
    class JsfxResourcesWindow {
        bool open_{false};
        bool paths_loaded_{false};
        bool use_defaults_{true};
        std::vector<std::string> search_paths_{};
        // A name the user invents for a set of effects inside our own storage -- not a
        // path they have to know, so it is typable on every platform.
        std::array<char, 128> destination_{};
        std::string status_{};
        bool status_is_error_{false};
        bool pick_in_flight_{false};
        // Set while the catalog is being rebuilt, which is the slow part: a registered
        // folder is re-read and re-copied before anything is scanned.
        bool busy_{false};

        // Registered folders, as the window last read them.
        std::vector<uapmd_jsfx::JsfxRegisteredFolder> registered_folders_{};

        void loadFromFormat();
        void registerPickedFolder(const uapmd::FolderPickResult& picked);
        void syncRegisteredFolders();
        void refreshCatalog();
        static const char* folderButtonLabel(bool foldersArePlaces);
        void applyToFormat();
        void beginFileImport();
        void beginFolderPick();
        void importDocuments(std::vector<uapmd::DocumentHandle> documents,
                             std::string subdirectory);
        void finishImport(uint32_t filesWritten, const std::vector<std::string>& failures);
        std::string destinationName() const;
        void report(std::string message, bool isError);

    public:
        bool isOpen() const { return open_; }
        void show() { open_ = true; paths_loaded_ = false; }
        void hide() { open_ = false; }
        void toggle() { if (open_) hide(); else show(); }

        void render();
    };

}

#endif
