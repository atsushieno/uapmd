#include <array>

#include <uapmd-addin-core/uapmd-addin-core.hpp>

namespace {

class DiagnosticsAddin final : public uapmd::Addin {
public:
    uapmd::AddinIdentity identity() const noexcept override {
        return {"org.uapmd.diagnostics", "lifecycle"};
    }

    std::string_view name() const noexcept override {
        return "Addin lifecycle diagnostics";
    }

    std::string_view path() const noexcept override {
        return "/uapmd/diagnostics/lifecycle/v1";
    }

    bool initialize(uapmd::AddinHost& host) noexcept override {
        initialized_ = true;
// This path intentionally has no concrete interface yet. Querying it
        // exercises the generic host boundary without retaining the result.
        (void) host.extensionPoint(path());
        return true;
    }

    void cleanup(uapmd::AddinHost&) noexcept override {
        initialized_ = false;
    }

private:
    bool initialized_ = false;
};

class DiagnosticsEntry final : public uapmd::AddinEntry {
public:
    DiagnosticsEntry() {
        addins_[0] = &addin_;
    }

    std::string_view packageId() const noexcept override {
        return "org.uapmd.diagnostics";
    }

    std::span<uapmd::Addin* const> addins() noexcept override {
        return addins_;
    }

private:
    DiagnosticsAddin addin_;
    std::array<uapmd::Addin*, 1> addins_{};
};

DiagnosticsEntry diagnosticsEntry;

} // namespace

extern "C" UAPMD_ADDIN_EXPORT uapmd::AddinEntry* uapmd_addin_entry() noexcept {
    return &diagnosticsEntry;
}
