#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#if defined(__ANDROID__)
#include <jni.h>
#endif

namespace uapmd {

// ─── Handle ──────────────────────────────────────────────────────────────────

// Opaque reference to a user-selected document.
//
// Desktop  : id is the filesystem path string.
// Android  : id is the content:// URI string (permission already held).
// Web      : id is a path in Emscripten's virtual filesystem (MEMFS).
//
// Never construct a DocumentHandle manually; always obtain one from
// IDocumentProvider::pickOpenDocuments, pickSaveDocument, or restoreHandle.
struct DocumentHandle {
    std::string id;           // Platform-internal identifier — treat as opaque
    std::string display_name; // Human-readable filename for UI labels
    std::string mime_type;    // MIME type if the platform reported it; else empty

    bool valid() const { return !id.empty(); }
};

// ─── Filters ─────────────────────────────────────────────────────────────────

// Describes a file-type category for the OS picker.
// Populate both fields; each platform uses what it understands:
//   mime_types → Android and Web (accept attribute)
//   extensions → Desktop pfd (e.g. "*.wav")
struct DocumentFilter {
    std::string label;
    std::vector<std::string> mime_types;
    std::vector<std::string> extensions;
};

// ─── Folders ─────────────────────────────────────────────────────────────────

// Returned by pickFolder.
//
// A folder is not the same kind of thing on every platform, and this carries
// whichever of the two a platform can give:
//
//   path      : a real filesystem path, which the caller may keep and read from
//               whenever it likes. Desktop only.
//   documents : the files that were inside the folder, as handles to read through
//               this interface. Android, iOS and Web hand out a grant or a browser
//               object rather than a location, so there is nothing to keep: the
//               content can be copied, but the folder cannot be referred to again.
//
// Exactly one of the two is populated on success. A caller that only understands
// paths does not work on three of the four platforms, which is why both are here.
//
// Cancelled by user : success == true,  path empty and documents empty.
// Error             : success == false, error is non-empty.
//
// display_name is the folder's own name, suitable for labelling it or for naming
// a copy of it. Each document's display_name is its path relative to the folder
// that was picked, so that copying them preserves the structure they had.
struct FolderPickResult {
    bool success = false;
    std::filesystem::path path;
    std::vector<DocumentHandle> documents;
    // A token that names this folder again later, to be stored and handed to
    // listFolderDocuments. Empty where a folder cannot be returned to at all, which
    // is the web: a browser gives up the files it held and nothing else.
    //
    // Holding a token may mean holding a grant -- on Android a persistable URI
    // permission, on iOS a bookmark -- so a token that is no longer wanted should be
    // dropped with releaseFolder() rather than merely forgotten.
    std::string token;
    std::string display_name;
    std::string error;
};

// ─── Result types ────────────────────────────────────────────────────────────

// Returned by pick operations.
// Cancelled by user : success == true,  handles is empty.
// Error             : success == false, error is non-empty.
struct DocumentPickResult {
    bool success = false;
    std::vector<DocumentHandle> handles;
    std::string error;
};

// Returned by I/O and path-resolution operations.
struct DocumentIOResult {
    bool success = false;
    std::string error;
};

// ─── Interface ───────────────────────────────────────────────────────────────

// Platform-agnostic interface for user-driven document access.
//
// Lifecycle:  pick → read/write (and/or resolveToPath) → persistHandle (optional)
//
// Threading:  Implementations may perform I/O on background threads internally,
//             but all callbacks are dispatched on the calling thread or the
//             platform's main/UI thread. Callers must not assume re-entrancy.
//
// Unsupported operations: all methods exist on every platform. Operations that
// cannot be honoured invoke their callback with success == false and a
// descriptive error string. They never throw.
class IDocumentProvider {
public:
    virtual ~IDocumentProvider() = default;

    static bool supportsCreateFileInternal();

    using PickCallback  = std::function<void(DocumentPickResult)>;
    using ReadCallback  = std::function<void(DocumentIOResult, std::vector<uint8_t>)>;
    using WriteCallback = std::function<void(DocumentIOResult)>;
    using PathCallback  = std::function<void(DocumentIOResult, std::filesystem::path)>;
    using FolderPickCallback = std::function<void(FolderPickResult)>;

    // ── Picking ──────────────────────────────────────────────────────────────

    // Show the OS file-open picker.
    // callback receives zero handles on cancel, one or more on success.
    // allowMultiple requests multi-selection (best-effort; not guaranteed everywhere).
    // On Android: URI permissions are taken inside the call before invoking callback.
    virtual void pickOpenDocuments(
        std::vector<DocumentFilter> filters,
        bool allowMultiple,
        PickCallback callback) = 0;

    // Show the OS folder picker.
    //
    // Whether a picked folder arrives as a path or as its contents is a property
    // of the platform rather than of the folder, so callers must handle both;
    // folderPathsAreUsable() answers which to expect before anything is picked,
    // for UI that has to say up front what the button is going to do.
    virtual void pickFolder(FolderPickCallback callback) = 0;

    // Whether pickFolder yields a path that stays valid after the call.
    // False where a folder is reached through a grant rather than a location; those
    // platforms answer with documents, and are re-read through listFolderDocuments.
    virtual bool folderPathsAreUsable() const = 0;

    // Re-reads a folder picked earlier, named by the token that pick produced.
    //
    // This is what makes a registered folder a standing arrangement rather than a
    // one-off copy: the user adds files to it with whatever tool they like, and the
    // next call sees them. The result is shaped exactly as pickFolder's.
    //
    // Fails when the grant has been revoked or the folder has gone, which the caller
    // should treat as "ask the user for it again" rather than as a hard error.
    virtual void listFolderDocuments(std::string token, FolderPickCallback callback) = 0;

    // Gives up a token obtained from pickFolder, releasing any grant behind it.
    // Callers must do this when the user removes a folder; forgetting the token alone
    // leaves the grant held for as long as the platform feels like it.
    virtual void releaseFolder(const std::string& token) = 0;

    // Show the OS save-file picker / save dialog.
    // callback receives exactly one handle on success, zero on cancel.
    // defaultName: suggested filename (leaf name only; no path separators).
    // On Web (no File System Access API): returns a handle immediately;
    //   writeDocument on that handle triggers a browser download instead.
    virtual void pickSaveDocument(
        std::string defaultName,
        std::vector<DocumentFilter> filters,
        PickCallback callback) = 0;

    // ── I/O ──────────────────────────────────────────────────────────────────

    // Read the complete document contents into memory.
    // Suitable for project files and plugin state (typically ≤ a few MB).
    virtual void readDocument(
        DocumentHandle handle,
        ReadCallback callback) = 0;

    // Overwrite the document with new data.
    virtual void writeDocument(
        DocumentHandle handle,
        std::vector<uint8_t> data,
        WriteCallback callback) = 0;

    // ── Path bridging ─────────────────────────────────────────────────────────

    // Resolve a handle to a filesystem path usable with std::fstream and
    // path-based library APIs (choc audio reader, UapmdProjectDataReader, etc.).
    //
    // Desktop : returns the path immediately — no copy performed.
    // Android : streams the content URI to a temp file; returns that path.
    // Web     : the virtual FS path is already in handle.id; returns it directly.
    //
    // Temp files (when created) are valid until resolveToPath is called again
    // for the same handle, or until the IDocumentProvider instance is destroyed.
    // Callers must not cache the returned path beyond that scope.
    //
    // Prefer readDocument + in-memory parsing over this where possible.
    virtual void resolveToPath(
        DocumentHandle handle,
        PathCallback callback) = 0;

    // ── Main-thread pump ─────────────────────────────────────────────────────────

    // Call once per frame from the application's main/render thread.
    // Implementations that run dialogs asynchronously (e.g. desktop with a
    // background pfd thread) use this to dispatch pending callbacks back to
    // the main thread so that ImGui and audio-engine state can be touched safely.
    // The default implementation is a no-op; override where needed.
    virtual void tick() {}

    // ── Persistence ───────────────────────────────────────────────────────────

    // Serialize a handle into a storable string token (e.g. for a recent-files list).
    //
    // Desktop : token is the filesystem path.
    // Android : token is the persisted content URI (permission already held).
    // Web     : returns "" — virtual FS paths do not survive a page reload.
    virtual std::string persistHandle(const DocumentHandle& handle) = 0;

    // Rebuild a handle from a token produced by persistHandle.
    // Returns nullopt if the file no longer exists, access was revoked, or
    // the token is from a different platform/session.
    virtual std::optional<DocumentHandle> restoreHandle(const std::string& token) = 0;
};

bool supportsCreateFileInternal();

inline bool IDocumentProvider::supportsCreateFileInternal()
{
    return ::uapmd::supportsCreateFileInternal();
}

// ─── Platform-specific factory and init ──────────────────────────────────────

#if defined(__ANDROID__)
// Must be called once before createDocumentProvider() on Android.
// env    : current JNIEnv (used to extract the JavaVM).
// activity : local or global ref to the host Activity.
void initDocumentProvider(JNIEnv* env, jobject activity);

// Wire this into your Activity's onActivityResult so the pick callbacks fire.
void documentProvider_onActivityResult(
    JNIEnv* env, int requestCode, int resultCode, jobject intent);
#endif

// Returns the platform-appropriate IDocumentProvider.
// On Android: requires initDocumentProvider() to have been called first.
std::unique_ptr<IDocumentProvider> createDocumentProvider();

} // namespace uapmd
