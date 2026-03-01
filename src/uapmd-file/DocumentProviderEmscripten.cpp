#include <uapmd-file/IDocumentProvider.hpp>

#include <emscripten.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <string>

namespace uapmd {

// ─── Callback registry ───────────────────────────────────────────────────────
//
// JS calls back into C++ via the exported functions below, keyed by an integer
// callbackId assigned when the pick is initiated.

namespace {

struct PendingPick {
    IDocumentProvider::PickCallback callback;
    bool allow_multiple;
};

std::mutex g_pick_mutex;
std::map<int, PendingPick> g_pending_picks;
std::atomic<int> g_next_id{1};

std::string tempPathForId(int id, const std::string& filename)
{
    return "/tmp/uapmd_pick_" + std::to_string(id) + "_" + filename;
}

} // anonymous namespace

// ─── Exported C bridge (called from JS) ──────────────────────────────────────

extern "C" {

// Called by JS when the user picks a file.
// The file has already been written to MEMFS at `vfsPath`.
EMSCRIPTEN_KEEPALIVE void uapmd_file_picked(int callbackId,
                                             const char* vfsPath,
                                             const char* displayName,
                                             const char* mimeType)
{
    PendingPick pick;
    {
        std::lock_guard lock(g_pick_mutex);
        auto it = g_pending_picks.find(callbackId);
        if (it == g_pending_picks.end()) return;
        pick = std::move(it->second);
        g_pending_picks.erase(it);
    }

    DocumentHandle h;
    h.id = vfsPath;
    h.display_name = displayName ? displayName : "";
    h.mime_type = mimeType ? mimeType : "";

    DocumentPickResult result;
    result.success = true;
    result.handles.push_back(std::move(h));
    pick.callback(std::move(result));
}

// Called by JS when the user cancels the picker.
EMSCRIPTEN_KEEPALIVE void uapmd_file_pick_cancelled(int callbackId)
{
    PendingPick pick;
    {
        std::lock_guard lock(g_pick_mutex);
        auto it = g_pending_picks.find(callbackId);
        if (it == g_pending_picks.end()) return;
        pick = std::move(it->second);
        g_pending_picks.erase(it);
    }
    // Cancelled → success, empty handles
    pick.callback({true, {}, {}});
}

} // extern "C"

// ─── Provider ────────────────────────────────────────────────────────────────

class DocumentProviderEmscripten : public IDocumentProvider {
    // Prefix on handle.id that marks a deferred-download save handle
    static constexpr const char* kSavePrefix = "save:";

public:
    void pickOpenDocuments(
        std::vector<DocumentFilter> filters,
        bool allowMultiple,
        PickCallback callback) override
    {
        int id = g_next_id.fetch_add(1);
        {
            std::lock_guard lock(g_pick_mutex);
            g_pending_picks[id] = {std::move(callback), allowMultiple};
        }

        // Build the accept string from MIME types and extensions
        std::string accept;
        for (auto& f : filters) {
            for (auto& m : f.mime_types) {
                if (!accept.empty()) accept += ',';
                accept += m;
            }
            for (auto& e : f.extensions) {
                // Convert "*.wav" → ".wav" for the accept attribute
                if (!accept.empty()) accept += ',';
                accept += (e.size() > 1 && e[0] == '*') ? e.substr(1) : e;
            }
        }

        // Create a hidden <input type=file>, trigger it, and write the chosen
        // file into MEMFS before calling back into C++.
        EM_ASM({
            var callbackId  = $0;
            var acceptStr   = UTF8ToString($1);
            var multiple    = $2;
            var prefix      = "/tmp/uapmd_pick_" + callbackId + "_";

            var input = document.createElement('input');
            input.type = 'file';
            if (acceptStr.length > 0) input.accept = acceptStr;
            if (multiple) input.multiple = true;
            input.style.display = 'none';
            document.body.appendChild(input);

            var cancelled = true;

            input.addEventListener('change', function(e) {
                cancelled = false;
                var files = e.target.files;
                if (!files || files.length === 0) {
                    Module.ccall('uapmd_file_pick_cancelled', null, ['number'], [callbackId]);
                    document.body.removeChild(input);
                    return;
                }

                // For simplicity, handle the first file and fire the callback.
                // Multiple-file support would loop here and collect all handles
                // before invoking the callback once.
                var file = files[0];
                var reader = new FileReader();
                reader.onload = function(re) {
                    var data = new Uint8Array(re.target.result);
                    var vfsPath = prefix + file.name;
                    FS.writeFile(vfsPath, data);

                    var vfsPathBuf  = allocateUTF8(vfsPath);
                    var nameBuf     = allocateUTF8(file.name);
                    var mimeBuf     = allocateUTF8(file.type || '');
                    Module.ccall('uapmd_file_picked', null,
                        ['number', 'number', 'number', 'number'],
                        [callbackId, vfsPathBuf, nameBuf, mimeBuf]);
                    _free(vfsPathBuf);
                    _free(nameBuf);
                    _free(mimeBuf);
                    document.body.removeChild(input);
                };
                reader.readAsArrayBuffer(file);
            });

            // Detect cancel via window focus regained without a change event
            window.addEventListener('focus', function onFocus() {
                window.removeEventListener('focus', onFocus);
                setTimeout(function() {
                    if (cancelled) {
                        Module.ccall('uapmd_file_pick_cancelled', null, ['number'], [callbackId]);
                        if (input.parentNode) document.body.removeChild(input);
                    }
                }, 500);
            }, { once: true });

            input.click();
        }, id, accept.c_str(), allowMultiple ? 1 : 0);
    }

    // Web has no native save dialog without the File System Access API.
    // We return a handle immediately; writeDocument triggers a browser download.
    void pickSaveDocument(
        std::string defaultName,
        std::vector<DocumentFilter> filters,
        PickCallback callback) override
    {
        DocumentHandle h;
        h.id = kSavePrefix + defaultName;
        h.display_name = defaultName;
        for (auto& f : filters)
            if (!f.mime_types.empty()) { h.mime_type = f.mime_types[0]; break; }

        callback({true, {std::move(h)}, {}});
    }

    void readDocument(DocumentHandle handle, ReadCallback callback) override
    {
        if (handle.id.starts_with(kSavePrefix)) {
            callback({false, "Cannot read from a save handle"}, {});
            return;
        }
        std::ifstream f(handle.id, std::ios::binary);
        if (!f) {
            callback({false, "File not found in virtual FS: " + handle.id}, {});
            return;
        }
        std::vector<uint8_t> data(
            (std::istreambuf_iterator<char>(f)),
            std::istreambuf_iterator<char>()
        );
        callback({true, {}}, std::move(data));
    }

    void writeDocument(DocumentHandle handle, std::vector<uint8_t> data, WriteCallback callback) override
    {
        if (handle.id.starts_with(kSavePrefix)) {
            // Trigger a browser download with the given data
            std::string filename = handle.id.substr(std::strlen(kSavePrefix));
            std::string mime = handle.mime_type.empty()
                ? "application/octet-stream"
                : handle.mime_type;

            EM_ASM({
                var ptr      = $0;
                var length   = $1;
                var mimeStr  = UTF8ToString($2);
                var nameStr  = UTF8ToString($3);

                var bytes = HEAPU8.subarray(ptr, ptr + length);
                var blob  = new Blob([bytes], {type: mimeStr});
                var url   = URL.createObjectURL(blob);
                var a     = document.createElement('a');
                a.href     = url;
                a.download = nameStr;
                document.body.appendChild(a);
                a.click();
                document.body.removeChild(a);
                URL.revokeObjectURL(url);
            }, data.data(), data.size(), mime.c_str(), filename.c_str());

            callback({true, {}});
            return;
        }

        // Write to virtual FS path
        std::ofstream f(handle.id, std::ios::binary | std::ios::trunc);
        if (!f) {
            callback({false, "Failed to write to virtual FS: " + handle.id});
            return;
        }
        f.write(reinterpret_cast<const char*>(data.data()),
                static_cast<std::streamsize>(data.size()));
        callback({true, {}});
    }

    // The virtual FS path in handle.id is already a real Emscripten MEMFS path.
    void resolveToPath(DocumentHandle handle, PathCallback callback) override
    {
        if (handle.id.starts_with(kSavePrefix)) {
            callback({false, "Cannot resolve a save handle to a path"}, {});
            return;
        }
        callback({true, {}}, std::filesystem::path(handle.id));
    }

    // Virtual FS paths do not survive a page reload.
    std::string persistHandle(const DocumentHandle&) override { return {}; }

    std::optional<DocumentHandle> restoreHandle(const std::string&) override
    {
        return std::nullopt;
    }
};

std::unique_ptr<IDocumentProvider> createDocumentProvider()
{
    return std::make_unique<DocumentProviderEmscripten>();
}

} // namespace uapmd
