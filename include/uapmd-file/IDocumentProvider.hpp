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

    using PickCallback  = std::function<void(DocumentPickResult)>;
    using ReadCallback  = std::function<void(DocumentIOResult, std::vector<uint8_t>)>;
    using WriteCallback = std::function<void(DocumentIOResult)>;
    using PathCallback  = std::function<void(DocumentIOResult, std::filesystem::path)>;

    // ── Picking ──────────────────────────────────────────────────────────────

    // Show the OS file-open picker.
    // callback receives zero handles on cancel, one or more on success.
    // allowMultiple requests multi-selection (best-effort; not guaranteed everywhere).
    // On Android: URI permissions are taken inside the call before invoking callback.
    virtual void pickOpenDocuments(
        std::vector<DocumentFilter> filters,
        bool allowMultiple,
        PickCallback callback) = 0;

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
