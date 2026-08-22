#include <array>

#include <uapmd-addin-core/uapmd-addin-core.hpp>

#include "BSRoformerStemSeparator.hpp"

using namespace uapmd_addin;

namespace {

constexpr std::string_view kStemSeparatorExtensionPoint{"/uapmd/audio-import/stem-separator/v1"};

class BSRoformerAddin final : public Addin {
public:
    AddinIdentity identity() const noexcept override {
        return {"/uapmd/bs-roformer", "stem-separator"};
    }

    std::string_view name() const noexcept override {
        return "BS-Roformer stem separation";
    }

    std::string_view path() const noexcept override {
        return kStemSeparatorExtensionPoint;
    }

    bool initialize(AddinHost& host) noexcept override {
        registry_ = static_cast<uapmd::import::StemSeparatorRegistry*>(
            host.extensionPoint(kStemSeparatorExtensionPoint));
        if (!registry_)
            return false;

        try {
            registry_->add(separator_);
            return true;
        } catch (...) {
            registry_ = nullptr;
            return false;
        }
    }

    void cleanup(AddinHost&) noexcept override {
        if (registry_)
            registry_->remove(separator_);
        registry_ = nullptr;
    }

private:
    uapmd_bsroformer::BSRoformerStemSeparator separator_{};
    uapmd::import::StemSeparatorRegistry* registry_{};
};

class BSRoformerAddinEntry final : public AddinEntry {
public:
    BSRoformerAddinEntry() {
        addins_[0] = &addin_;
    }

    std::string_view packageId() const noexcept override {
        return "/uapmd/bs-roformer";
    }

    std::span<Addin* const> addins() noexcept override {
        return addins_;
    }

private:
    BSRoformerAddin addin_;
    std::array<Addin*, 1> addins_{};
};

BSRoformerAddinEntry bsRoformerAddinEntry;

// Built-in addin, like Demucs: linked into the application rather than loaded
// from the addin directory, so the entry announces itself from a static
// initializer and the application links it WHOLE_ARCHIVE.
class BSRoformerBuiltinAddinRegistration final {
public:
    BSRoformerBuiltinAddinRegistration() {
        registerBuiltinAddin(bsRoformerAddinEntry);
    }
};

BSRoformerBuiltinAddinRegistration bsRoformerBuiltinAddinRegistration;

} // namespace
