#include <uapmd-file/IDocumentProvider.hpp>
#include <portable-file-dialogs.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace uapmd {

namespace {

// Build a pfd-compatible filter list from DocumentFilters.
// pfd expects alternating {"Label", "*.ext1 *.ext2"} pairs.
std::vector<std::string> buildPfdFilters(const std::vector<DocumentFilter>& filters)
{
    std::vector<std::string> out;
    for (auto& f : filters) {
        std::string exts;
        for (auto& e : f.extensions) {
            if (!exts.empty()) exts += ' ';
            exts += e;
        }
        if (exts.empty()) exts = "*";
        out.push_back(f.label);
        out.push_back(exts);
    }
    if (out.empty()) {
        out = {"All Files", "*"};
    }
    return out;
}

DocumentHandle handleFromPath(const std::string& path)
{
    DocumentHandle h;
    h.id = path;
    h.display_name = std::filesystem::path(path).filename().string();
    return h;
}

} // anonymous namespace

// On macOS pfd uses osascript (a subprocess). Calling pfd::open_file::result()
// blocks in a waitpid() poll loop which freezes SDL's event loop, preventing
// the Cocoa/NSApp run loop from processing events — making the native file
// dialog appear unresponsive.
//
// Fix: each pick operation is run on a detached background thread. When the
// dialog completes the pending callback is pushed into a mutex-protected queue.
// tick() (called once per frame from MainWindow::update) drains the queue on
// the main thread, so ImGui and audio-engine state can be touched safely.

class DocumentProviderDesktop : public IDocumentProvider {

    std::mutex              queue_mutex_;
    std::queue<std::function<void()>> pending_;

    void enqueue(std::function<void()> fn) {
        std::lock_guard<std::mutex> lk(queue_mutex_);
        pending_.push(std::move(fn));
    }

public:

    // ── Picking ──────────────────────────────────────────────────────────────

    void pickOpenDocuments(
        std::vector<DocumentFilter> filters,
        bool allowMultiple,
        PickCallback callback) override
    {
        auto pfd_filters = buildPfdFilters(filters);
        auto opt = allowMultiple ? pfd::opt::multiselect : pfd::opt::none;

        std::thread([this,
                     pfd_filters = std::move(pfd_filters),
                     opt,
                     callback = std::move(callback)]() mutable
        {
            pfd::open_file dialog("Open", ".", pfd_filters, opt);

            DocumentPickResult result;
            result.success = true;
            for (auto& path : dialog.result())
                result.handles.push_back(handleFromPath(path));

            enqueue([callback = std::move(callback),
                     result   = std::move(result)]() mutable {
                callback(std::move(result));
            });
        }).detach();
    }

    void pickSaveDocument(
        std::string defaultName,
        std::vector<DocumentFilter> filters,
        PickCallback callback) override
    {
        auto pfd_filters = buildPfdFilters(filters);

        std::thread([this,
                     defaultName  = std::move(defaultName),
                     pfd_filters  = std::move(pfd_filters),
                     callback     = std::move(callback)]() mutable
        {
            pfd::save_file dialog("Save", defaultName, pfd_filters);

            DocumentPickResult result;
            result.success = true;
            auto path = dialog.result();
            if (!path.empty())
                result.handles.push_back(handleFromPath(path));

            enqueue([callback = std::move(callback),
                     result   = std::move(result)]() mutable {
                callback(std::move(result));
            });
        }).detach();
    }

    // ── I/O ──────────────────────────────────────────────────────────────────

    void readDocument(DocumentHandle handle, ReadCallback callback) override
    {
        std::ifstream f(handle.id, std::ios::binary);
        if (!f) {
            callback({false, "Failed to open: " + handle.id}, {});
            return;
        }
        std::vector<uint8_t> data(
            (std::istreambuf_iterator<char>(f)),
            std::istreambuf_iterator<char>()
        );
        callback({true, {}}, std::move(data));
    }

    void writeDocument(DocumentHandle handle,
                       std::vector<uint8_t> data,
                       WriteCallback callback) override
    {
        std::ofstream f(handle.id, std::ios::binary | std::ios::trunc);
        if (!f) {
            callback({false, "Failed to open for writing: " + handle.id});
            return;
        }
        f.write(reinterpret_cast<const char*>(data.data()),
                static_cast<std::streamsize>(data.size()));
        callback({true, {}});
    }

    // ── Path bridging ─────────────────────────────────────────────────────────

    void resolveToPath(DocumentHandle handle, PathCallback callback) override
    {
        // On desktop the id is already the filesystem path.
        callback({true, {}}, std::filesystem::path(handle.id));
    }

    // ── Persistence ───────────────────────────────────────────────────────────

    std::string persistHandle(const DocumentHandle& handle) override
    {
        return handle.id; // id is the filesystem path
    }

    std::optional<DocumentHandle> restoreHandle(const std::string& token) override
    {
        if (token.empty() || !std::filesystem::exists(token))
            return std::nullopt;
        return handleFromPath(token);
    }

    // ── Main-thread pump ─────────────────────────────────────────────────────

    void tick() override
    {
        // Collect pending callbacks under the lock, then release before calling
        // them so that a callback that itself triggers a pick doesn't deadlock.
        std::queue<std::function<void()>> ready;
        {
            std::lock_guard<std::mutex> lk(queue_mutex_);
            std::swap(ready, pending_);
        }
        while (!ready.empty()) {
            ready.front()();
            ready.pop();
        }
    }
};

std::unique_ptr<IDocumentProvider> createDocumentProvider()
{
    return std::make_unique<DocumentProviderDesktop>();
}

} // namespace uapmd
