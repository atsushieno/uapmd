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

// Folder picks are collected a file at a time -- the browser hands over a flat
// FileList and each one is read asynchronously -- so the documents accumulate
// here until JS says the folder is complete.
struct PendingFolderPick {
    IDocumentProvider::FolderPickCallback callback;
    std::vector<DocumentHandle> documents;
};

std::mutex g_folder_mutex;
std::map<int, PendingFolderPick> g_pending_folder_picks;

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

// Called by JS once per file found inside the chosen folder. The file is already
// in MEMFS at `vfsPath`; `relativePath` is its path within the folder.
EMSCRIPTEN_KEEPALIVE void uapmd_folder_document(int callbackId,
                                                const char* vfsPath,
                                                const char* relativePath,
                                                const char* mimeType)
{
    std::lock_guard lock(g_folder_mutex);
    auto it = g_pending_folder_picks.find(callbackId);
    if (it == g_pending_folder_picks.end()) return;

    DocumentHandle h;
    h.id = vfsPath;
    h.display_name = relativePath ? relativePath : "";
    h.mime_type = mimeType ? mimeType : "";
    it->second.documents.push_back(std::move(h));
}

// Called by JS when every file of the chosen folder has been handed over.
// A null or empty folderName means the user cancelled.
EMSCRIPTEN_KEEPALIVE void uapmd_folder_picked(int callbackId, const char* folderName)
{
    PendingFolderPick pick;
    {
        std::lock_guard lock(g_folder_mutex);
        auto it = g_pending_folder_picks.find(callbackId);
        if (it == g_pending_folder_picks.end()) return;
        pick = std::move(it->second);
        g_pending_folder_picks.erase(it);
    }
    if (!pick.callback) return;

    FolderPickResult result;
    result.success = true;
    result.display_name = folderName ? folderName : "";
    result.documents = std::move(pick.documents);
    pick.callback(std::move(result));
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

            // A dismissed dialog fires 'cancel'; the focus signal below is the old
            // way of guessing, and it mistakes a slow selection for a cancelled one.
            function cancelPick() {
                if (!cancelled) return;
                cancelled = false;
                Module.ccall('uapmd_file_pick_cancelled', null, ['number'], [callbackId]);
                if (input.parentNode) document.body.removeChild(input);
            }
            if ('oncancel' in input)
                input.addEventListener('cancel', cancelPick);
            else
                window.addEventListener('focus', function onFocus() {
                    window.removeEventListener('focus', onFocus);
                    setTimeout(cancelPick, 30000);
                }, { once: true });

            input.click();
        }, id, accept.c_str(), allowMultiple ? 1 : 0);
    }

    void pickFolder(FolderPickCallback callback) override
    {
        int id = g_next_id.fetch_add(1);
        {
            std::lock_guard lock(g_folder_mutex);
            g_pending_folder_picks[id] = {std::move(callback), {}};
        }

        // A directory <input> hands over a flat list of files, each carrying the
        // path it had inside the chosen folder. That is as close to a folder as a
        // browser gets: there is nothing to keep afterwards, only content to copy.
        EM_ASM({
            var callbackId = $0;
            var root = "/tmp/uapmd_folder_" + callbackId;

            var input = document.createElement('input');
            input.type = 'file';
            input.webkitdirectory = true;
            input.multiple = true;
            input.style.display = 'none';
            document.body.appendChild(input);

            var finished = false;
            var delivered = 0;
            function finish(folderName) {
                if (finished) return;
                finished = true;
                console.log('[uapmd] Folder pick finished: ' + delivered +
                            ' file(s) handed over' +
                            (folderName ? ' from "' + folderName + '".' : ', nothing chosen.'));
                var nameBuf = allocateUTF8(folderName || '');
                Module.ccall('uapmd_folder_picked', null, ['number', 'number'],
                             [callbackId, nameBuf]);
                _free(nameBuf);
                // Every document has been read during that call and copied where it
                // belongs, so what was staged here is now a second copy of the whole
                // folder taking up memory.
                removeRecursively(root);
                if (input.parentNode) document.body.removeChild(input);
            }

            function removeRecursively(path) {
                var entries;
                try { entries = FS.readdir(path); } catch (e) { return; }
                for (var i = 0; i < entries.length; i++) {
                    var name = entries[i];
                    if (name === '.' || name === '..') continue;
                    var child = path + '/' + name;
                    var mode;
                    try { mode = FS.stat(child).mode; } catch (e) { continue; }
                    if (FS.isDir(mode)) removeRecursively(child);
                    else { try { FS.unlink(child); } catch (e) { } }
                }
                try { FS.rmdir(path); } catch (e) { }
            }

            function mkdirp(path) {
                var parts = path.split('/').filter(function(p) { return p.length > 0; });
                var current = '';
                for (var i = 0; i < parts.length; i++) {
                    current += '/' + parts[i];
                    try { FS.mkdir(current); } catch (e) { /* already there */ }
                }
            }

            input.addEventListener('change', function(e) {
                var files = e.target.files;
                if (!files || files.length === 0) { finish(''); return; }

                var folderName = '';
                var first = files[0].webkitRelativePath || '';
                if (first.indexOf('/') > 0) folderName = first.split('/')[0];

                // Version control and package manager bookkeeping is not content, and a
                // directory upload hands it over with everything else: a .git directory
                // is usually the largest thing in a folder of effects, and never holds
                // one. The other platforms' walks skip these too.
                var wanted = [];
                for (var i = 0; i < files.length; i++) {
                    var full = files[i].webkitRelativePath || files[i].name;
                    if (full.charAt(0) === '.' || full.indexOf('/.') >= 0) continue;
                    wanted.push(files[i]);
                }
                console.log('[uapmd] Folder "' + folderName + '": ' + files.length +
                            ' file(s) offered, ' + wanted.length + ' to read, ' +
                            (files.length - wanted.length) + ' hidden and skipped.');
                if (wanted.length === 0) { finish(folderName); return; }

                // One file at a time. Starting a reader for every file at once holds
                // the entire folder in memory as ArrayBuffers while the copies below
                // are still being written, which a folder of any size does not survive.
                var index = 0;
                function readNext() {
                    if (index >= wanted.length) { finish(folderName); return; }
                    var file = wanted[index++];
                    var relative = file.webkitRelativePath || file.name;
                    // The folder's own name leads every path; the caller wants the part
                    // below it.
                    var slash = relative.indexOf('/');
                    if (slash >= 0) relative = relative.substring(slash + 1);

                    var reader = new FileReader();
                    reader.onload = function(re) {
                        try {
                            var vfsPath = root + '/' + relative;
                            mkdirp(vfsPath.substring(0, vfsPath.lastIndexOf('/')));
                            FS.writeFile(vfsPath, new Uint8Array(re.target.result));
                            var pathBuf = allocateUTF8(vfsPath);
                            var relBuf  = allocateUTF8(relative);
                            var mimeBuf = allocateUTF8(file.type || '');
                            Module.ccall('uapmd_folder_document', null,
                                ['number', 'number', 'number', 'number'],
                                [callbackId, pathBuf, relBuf, mimeBuf]);
                            _free(pathBuf);
                            _free(relBuf);
                            _free(mimeBuf);
                            delivered++;
                            console.log('[uapmd] Uploaded ' + relative + ' (' +
                                        re.target.result.byteLength + ' bytes)');
                        } catch (err) {
                            // One file that will not write is not worth losing the
                            // folder over, but it should not be silent either.
                            console.warn('[uapmd] Skipped ' + relative + ': ' + err);
                        }
                        readNext();
                    };
                    reader.onerror = function() { readNext(); };
                    reader.readAsArrayBuffer(file);
                }
                readNext();
            });

            // A dismissed dialog fires 'cancel'. Guessing from window focus instead
            // does not work here: focus comes back as soon as the upload is confirmed,
            // while the browser is still enumerating the directory, so a folder of any
            // size looks exactly like a cancelled one and its files arrive after the
            // pick has already been answered and forgotten.
            if ('oncancel' in input)
                input.addEventListener('cancel', function() { finish(''); });
            else
                // Older browsers have only the focus signal. The wait has to outlast
                // enumerating a large folder, and only ever ends a pick that really did
                // choose nothing.
                window.addEventListener('focus', function onFocus() {
                    window.removeEventListener('focus', onFocus);
                    setTimeout(function() {
                        if (!input.files || input.files.length === 0) finish('');
                    }, 30000);
                }, { once: true });

            input.click();
        }, id);
    }

    // MEMFS paths exist, but the folder the user chose does not: a browser hands
    // over the files it contained, not a place that can be read again.
    bool folderPathsAreUsable() const override { return false; }

    // And there is no token either: a directory <input> is a one-off transfer, with
    // nothing on the other side to come back to. Effects have to be copied in here.
    void listFolderDocuments(std::string, FolderPickCallback callback) override
    {
        FolderPickResult result;
        result.error = "A browser cannot re-read a folder; copy the effects in instead.";
        callback(std::move(result));
    }

    void releaseFolder(const std::string&) override {}

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

                // Blob cannot be constructed directly from a view backed by the
                // shared wasm heap, so make a detached copy first.
                var bytes = HEAPU8.slice(ptr, ptr + length);
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

bool supportsCreateFileInternal()
{
    return false;
}

} // namespace uapmd
