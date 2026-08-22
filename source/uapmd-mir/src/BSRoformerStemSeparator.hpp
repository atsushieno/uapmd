#pragma once

#include <uapmd-data/uapmd-data.hpp>

namespace uapmd_bsroformer {

// BSRoformer.cpp-backed implementation of the stem separation extension point,
// covering the BS-Roformer and Mel-Band-Roformer models. The model file is a
// GGUF conversion, supplied per request.
class BSRoformerStemSeparator final : public uapmd::import::StemSeparator {
public:
    std::string_view id() const noexcept override;
    std::string_view name() const noexcept override;
    uapmd::import::StemSeparatorModelFileSpec modelFileSpec() const override;
    uapmd::import::StemSeparationResult separate(
        const uapmd::import::StemSeparationRequest& request) const override;
};

} // namespace uapmd_bsroformer
