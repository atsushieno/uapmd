#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "uapmd-plugin-hosting/uapmd-plugin-hosting.hpp"

namespace uapmd_jsfx {

    // Gives the JSFX editor a font to draw with.
    //
    // On Windows, macOS and Linux the editor uses the platform's own fonts, exactly as
    // REAPER does, and this does nothing. Elsewhere there is no platform font stack to
    // borrow, so the application supplies TrueType data of its own -- whatever it already
    // ships for its own UI will do. The data is copied.
    //
    // Calling it is optional. Without it, JSFX text is drawn with the small bitmap font
    // built into LICE: legible, fixed-width, ASCII only.
    //
    // Call it before opening any editor; it does not affect editors already open.
    void setJsfxEditorFontData(const void* data, size_t size);

    // Whether this build draws JSFX editor text itself rather than using the platform's
    // fonts, i.e. whether setJsfxEditorFontData() has any effect.
    bool jsfxUsesOwnFontBackend();

    // Where effects the user adds themselves are kept.
    //
    // JSFX is a filesystem format: ysfx opens scripts, their includes and their data by
    // path, so anything the user imports has to end up as real files somewhere writable.
    // This is that place, and it is always one of the search paths.
    //
    // On desktop it sits beside the plugin caches. On Android it is app storage, because
    // the shared areas need permissions the application does not ask for. On the web it is
    // the IDBFS-backed upload directory, so imports survive a reload.
    //
    // Returns an empty path where there is nowhere writable, in which case importing is
    // unavailable and only the platform's own locations are searched.
    std::filesystem::path jsfxUserContentDirectory();

    // Whether a file should be treated as an archive to unpack rather than a single
    // effect to copy. Decided by name, because that is all a picked document reliably has.
    bool looksLikeJsfxArchive(const std::string& fileName);

    struct JsfxImportResult {
        bool success{false};
        uint32_t filesWritten{0};
        std::filesystem::path destination{};
        std::string error{};   // empty when success
    };

    // Puts imported content into the user content directory, either copying a single file
    // or unpacking an archive. `subdirectory` is optional and lets a set of effects keep
    // its own folder; it must stay inside the content directory.
    //
    // Archives are untrusted: an entry that would be written outside the destination
    // aborts the whole import rather than being skipped quietly.
    //
    // The caller supplies the bytes, so this works the same whether they came from a file
    // on disk, a document the user picked through the platform's own picker, or a browser
    // upload. A rescan is needed afterwards for the new effects to reach the catalog.
    JsfxImportResult importJsfxContent(const std::string& fileName,
                                       const uint8_t* data,
                                       size_t size,
                                       const std::string& subdirectory = {});

    // ─── Registered folders ──────────────────────────────────────────────────
    //
    // A folder the user has handed over for good, on a platform where a folder is a
    // grant rather than a location: Android's persisted tree permission, iOS's
    // bookmark. The user keeps their effects where they put them -- Download/JSFX,
    // say -- adds to that folder whenever they like with whatever tool they like, and
    // this host keeps up.
    //
    // Keeping up means copying, because ysfx opens scripts, their imports and their
    // data files by path and a grant has no path behind it. Each registered folder is
    // therefore mirrored into a directory of our own that is searched, and the mirror
    // is rebuilt whenever the folder is re-read. JSFX content is text-sized -- REAPER's
    // entire Effects folder is about 1.5 MB -- so the duplication is cheap in a way it
    // would not be for a sample library.
    //
    // Desktop does not use any of this: a folder there is a path, and goes straight
    // into the search paths.
    struct JsfxRegisteredFolder {
        std::string token;   // opaque; means something only to the IDocumentProvider
        std::string name;    // the user-facing name, and the mirror's directory name
    };

    // Where every mirror lives. It is always one of the search paths.
    std::filesystem::path jsfxFolderMirrorRoot();

    std::vector<JsfxRegisteredFolder> jsfxRegisteredFolders();

    // Registering the same token twice renames the existing entry rather than adding
    // another. Returns the name actually used, which differs from the one asked for
    // when that name is taken or unusable.
    std::string registerJsfxFolder(const std::string& token, const std::string& name);

    // Forgets the folder and deletes its mirror. The caller is responsible for giving
    // the grant itself back through IDocumentProvider::releaseFolder.
    void unregisterJsfxFolder(const std::string& token);

    // Empties a registered folder's mirror, ready for it to be written again. Files
    // the user removed from the folder have to stop being effects here too, which a
    // copy on top of the old mirror would not achieve.
    bool clearJsfxFolderMirror(const std::string& name);

    // Writes one file of a registered folder into its mirror. `relativePath` is the
    // path the file had inside the folder, and must stay inside the mirror.
    JsfxImportResult writeJsfxFolderMirrorFile(const std::string& name,
                                               const std::string& relativePath,
                                               const uint8_t* data,
                                               size_t size);

    // JSFX, the effects language REAPER ships, hosted by ysfx.
    //
    // This is an application-provided plugin format: construct one, hand it to
    // AudioPluginHostingAPI::addPluginFormat() before the first scan, and keep it alive for
    // as long as the host is used. Its effects then appear in the plugin catalog and
    // instantiate like any other format's.
    //
    // Nothing in uapmd-plugin-hosting knows about JSFX, and nothing here exposes ysfx, so
    // an application that links this module does not inherit ysfx's headers.
    class JsfxPluginFormat : public uapmd_plugin_hosting::AudioPluginFormat {
        struct Impl;
        std::unique_ptr<Impl> impl_;

    public:
        JsfxPluginFormat();
        ~JsfxPluginFormat() override;

        std::string name() override;

        // JSFX has no main thread affinity. Effects are compiled and run wherever they are
        // asked to be, which is unusual among plugin formats and worth taking advantage of.
        uapmd_plugin_hosting::AudioPluginUIThreadRequirement requiresUIThreadOn(
                uapmd_plugin_hosting::AudioPluginCatalogEntry* entry) override;

        uapmd_plugin_hosting::AudioPluginScanning* scanning() override;

        void createInstance(
                uapmd_plugin_hosting::AudioPluginCatalogEntry* entry,
                const uapmd_plugin_hosting::AudioPluginInstantiationOptions& options,
                std::function<void(std::unique_ptr<uapmd_plugin_hosting::AudioPluginInstanceAPI> instance,
                                   std::string error)> callback) override;

        // The locations searched in addition to, or instead of, the platform's
        // conventional ones. Set these before scanning; changing them afterwards requires a
        // rescan, because removing a location has to remove its effects from the catalog.
        void searchPaths(std::vector<std::string> paths, bool alsoUseDefaults);
        std::vector<std::string> searchPaths() const;
        bool useDefaultSearchPaths() const;

        // Where JSFX effects conventionally live on this platform. It is empty on Android,
        // iOS and the web, which have no REAPER installation to borrow from; there the
        // application supplies a location of its own through searchPaths().
        std::vector<std::filesystem::path> defaultSearchPaths() const;
    };

}
