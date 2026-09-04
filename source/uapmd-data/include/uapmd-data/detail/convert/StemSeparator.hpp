#pragma once

#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace uapmd::import {

// A separated stem written to disk by a StemSeparator.
struct StemFile {
    std::string name;
    std::filesystem::path filepath;
};

struct StemSeparationResult {
    bool success{false};
    bool canceled{false};
    std::string error;
    std::vector<StemFile> stems;
};

// What kind of model file the separator needs from the user, so that a host UI
// can present a file picker without knowing which separator it is talking to.
// An empty `extensions` list means the separator needs no model file at all.
struct StemSeparatorModelFileSpec {
    std::string label;
    std::vector<std::string> extensions;

    bool required() const { return !extensions.empty(); }
};

// Return false from the progress callback to cancel the separation.
using StemSeparationProgressCallback = std::function<bool(float /*progress*/, const std::string& /*message*/)>;
using StemSeparationCancelCallback = std::function<bool()>;

struct StemSeparationRequest {
    std::string audioFile;
    std::filesystem::path outputDirectory;
    // Interpreted by the separator, following its modelFileSpec().
    std::string modelPath;
    StemSeparationProgressCallback progressCallback;
    StemSeparationCancelCallback shouldCancel;
};

// A stem separation backend. Implementations are contributed by addins through
// the `/uapmd/audio-import/stem-separator/v1` extension point; the registry
// only borrows them, so an addin must remove its separator during cleanup().
//
// separate() runs on a worker thread and must honor the request's cancellation
// callbacks. It is never called on the audio thread.
class StemSeparator {
public:
    virtual ~StemSeparator() = default;

    virtual std::string_view id() const noexcept = 0;
    virtual std::string_view name() const noexcept = 0;
    virtual StemSeparatorModelFileSpec modelFileSpec() const = 0;
    virtual StemSeparationResult separate(const StemSeparationRequest& request) const = 0;
};

// Separators are contributed, not replaced: an addin adds its own backend
// alongside whatever else is registered. Registration order is preserved so
// that a host UI can offer the first one as its default.
//
// A separator is owned by the addin that contributed it, and disabling that
// addin unloads the code behind it. Anything that calls separate() off the UI
// thread must therefore hold a Lease for the whole run; withdrawal waits for
// outstanding leases instead of pulling the object out from under them.
class StemSeparatorRegistry {
public:
    // Keeps a separator usable across a separation run. Poll withdrawn() from
    // the run's cancellation callback: the wait in remove() lasts only as long
    // as the run takes to notice.
    class Lease {
    public:
        Lease() noexcept = default;
        ~Lease();
        Lease(Lease&& other) noexcept;
        Lease& operator=(Lease&& other) noexcept;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;

        StemSeparator* get() const noexcept { return separator_; }
        StemSeparator* operator->() const noexcept { return separator_; }
        explicit operator bool() const noexcept { return separator_ != nullptr; }

        // True once the separator is being withdrawn: stop the run.
        bool withdrawn() const noexcept;
        void release() noexcept;

    private:
        friend class StemSeparatorRegistry;
        Lease(StemSeparatorRegistry& registry, StemSeparator& separator) noexcept;

        StemSeparatorRegistry* registry_{};
        StemSeparator* separator_{};
    };

    void add(StemSeparator& separator);
    // Withdraws `separator` and blocks until every lease on it is released.
    bool remove(StemSeparator& separator) noexcept;

    // Snapshots for a host UI. These borrow without a lease, so they are only
    // safe on the thread that also drives addin enablement (the UI thread).
    std::vector<StemSeparator*> separators() const;
    StemSeparator* get(std::string_view id) const;
    bool empty() const noexcept;

    // Returns an empty lease when no such separator is registered, or when it
    // is already being withdrawn.
    Lease acquire(std::string_view id);

private:
    struct Entry {
        StemSeparator* separator{};
        int leases{0};
        bool withdrawn{false};
    };

    const Entry* findEntry(const StemSeparator& separator) const;
    Entry* findEntry(const StemSeparator& separator);
    void releaseLease(StemSeparator& separator) noexcept;
    bool isWithdrawn(const StemSeparator& separator) const noexcept;

    mutable std::mutex mutex_;
    std::condition_variable lease_released_;
    std::vector<Entry> entries_{};
};

} // namespace uapmd::import
