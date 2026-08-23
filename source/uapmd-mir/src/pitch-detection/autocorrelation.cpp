// Substitute for the same file in sevagh/pitch-detection (MIT, see NOTICE).
//
// This is the one file that had to be rewritten rather than reused: upstream
// runs the Wiener-Khinchin autocorrelation through FFTS, which this project
// does not build. pocketfft is header-only and takes arbitrary transform
// lengths, so the power-of-two special case upstream carries for FFTS is gone
// -- every window size goes down the real-to-complex path.

#include "pitch_detection.h"

#include <pocketfft_hdronly.h>

#include <algorithm>
#include <complex>
#include <cstddef>
#include <vector>

namespace {

pocketfft::shape_t makeShape(long n) {
    return pocketfft::shape_t{static_cast<std::size_t>(n)};
}

pocketfft::stride_t strideReal() {
    return pocketfft::stride_t{static_cast<std::ptrdiff_t>(sizeof(float))};
}

pocketfft::stride_t strideComplex() {
    return pocketfft::stride_t{
        static_cast<std::ptrdiff_t>(sizeof(std::complex<float>))};
}

} // namespace

template <typename T>
void util::acorr_r(const std::vector<T>& audio_buffer, pitch_alloc::BaseAlloc* ba) {
    std::copy(audio_buffer.begin(), audio_buffer.begin() + ba->nfft,
              ba->out_real.begin());

    pocketfft::r2c(makeShape(ba->nfft), strideReal(), strideComplex(),
                   /*axis=*/0, /*forward=*/true,
                   ba->out_real.data(), ba->out_im.data(), /*fct=*/1.0f);

    // Power spectrum, scaled so that the inverse transform below lands on the
    // same magnitudes upstream produced.
    const auto scale = 1.0f / static_cast<float>(ba->nfft);
    for (auto& bin : ba->out_im)
        bin = bin * std::conj(bin) * scale;

    pocketfft::c2r(makeShape(ba->nfft), strideComplex(), strideReal(),
                   /*axis=*/0, /*forward=*/false,
                   ba->out_im.data(), ba->out_real.data(), /*fct=*/1.0f);
}

template void util::acorr_r<double>(
    const std::vector<double>& audio_buffer, pitch_alloc::BaseAlloc* ba);

template void util::acorr_r<float>(
    const std::vector<float>& audio_buffer, pitch_alloc::BaseAlloc* ba);
