#pragma once

#include <uapmd-data/uapmd-data.hpp>

namespace uapmd_demucs {

// demucs.cpp-backed implementation of the stem separation extension point.
// The model file is a demucs.cpp ggml model, supplied per request.
class DemucsStemSeparator final : public uapmd::import::StemSeparator {
public:
    std::string_view id() const noexcept override;
    std::string_view name() const noexcept override;
    uapmd::import::StemSeparatorModelFileSpec modelFileSpec() const override;
    uapmd::import::StemSeparationResult separate(
        const uapmd::import::StemSeparationRequest& request) const override;
};

} // namespace uapmd_demucs
