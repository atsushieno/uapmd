// The single entry point of the uapmd-mir addin library.
//
// The library always carries pitch transcription. The music-analysis addins are
// compiled in only when UAPMD_ENABLE_MIR is set, because librosa.cpp and
// libsonare are not fetched otherwise. This file is the one place that knows
// which of them exist, so that the exported entry is present either way.

#include <span>
#include <vector>

#include <uapmd-addin-core/uapmd-addin-core.hpp>

using namespace uapmd_addin;

extern Addin* uapmd_pitch_transcription_addin() noexcept;
#if UAPMD_ENABLE_MIR_ANALYSIS
extern Addin* uapmd_mir_analysis_addin() noexcept;
extern Addin* uapmd_mir_librosa_addin() noexcept;
#endif

namespace {

class UapmdMirEntry final : public AddinEntry {
public:
    UapmdMirEntry() {
        addins_.push_back(uapmd_pitch_transcription_addin());
#if UAPMD_ENABLE_MIR_ANALYSIS
        addins_.push_back(uapmd_mir_analysis_addin());
        addins_.push_back(uapmd_mir_librosa_addin());
#endif
    }

    std::string_view packageId() const noexcept override {
        return "/uapmd/mir";
    }

    std::span<Addin* const> addins() noexcept override {
        return addins_;
    }

private:
    std::vector<Addin*> addins_;
};

UapmdMirEntry uapmdMirEntry;

} // namespace

extern "C" UAPMD_ADDIN_EXPORT AddinEntry* uapmd_addin_entry() noexcept {
    return &uapmdMirEntry;
}
