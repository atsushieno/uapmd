#include <array>

#include <uapmd-addin-core/uapmd-addin-core.hpp>

#include "DemucsStemSeparator.hpp"

using namespace uapmd_addin;

namespace {

constexpr std::string_view kStemSeparatorExtensionPoint{"/uapmd/audio-import/stem-separator/v1"};

class DemucsAddin final : public Addin {
public:
    AddinIdentity identity() const noexcept override {
        return {"/uapmd/demucs", "stem-separator"};
    }

    std::string_view name() const noexcept override {
        return "Demucs stem separation";
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
    uapmd_demucs::DemucsStemSeparator separator_{};
    uapmd::import::StemSeparatorRegistry* registry_{};
};

class DemucsAddinEntry final : public AddinEntry {
public:
    DemucsAddinEntry() {
        addins_[0] = &addin_;
    }

    std::string_view packageId() const noexcept override {
        return "/uapmd/demucs";
    }

    std::span<Addin* const> addins() noexcept override {
        return addins_;
    }

private:
    DemucsAddin addin_;
    std::array<Addin*, 1> addins_{};
};

DemucsAddinEntry demucsAddinEntry;

// Stem separation is a built-in addin: the library is linked into the
// application rather than loaded from the addin directory, so the entry
// announces itself from a static initializer the way ARA support does. The
// application links it WHOLE_ARCHIVE to keep this object file.
class DemucsBuiltinAddinRegistration final {
public:
    DemucsBuiltinAddinRegistration() {
        registerBuiltinAddin(demucsAddinEntry);
    }
};

DemucsBuiltinAddinRegistration demucsBuiltinAddinRegistration;

} // namespace
