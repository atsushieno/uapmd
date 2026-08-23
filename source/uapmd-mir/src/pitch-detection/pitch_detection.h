#pragma once

// Replacement for the public header of sevagh/pitch-detection.
//
// Upstream's own `include/pitch_detection.h` cannot be used here: it includes
// <mlpack/core.hpp> and stores an mlpack HMM inside the base class that every
// estimator derives from, so mlpack -- and Armadillo, and a BLAS -- would be
// mandatory even for the plain estimators. It also includes <ffts/ffts.h> and
// keeps FFTS plan handles as members. Neither library can follow this project
// to WebAssembly and Android.
//
// Upstream's mpm.cpp and yin.cpp reach the HMM only through the `hmm` member
// and util::pitch_from_hmm(), both declared here, so declaring an opaque
// placeholder lets those two files compile **unmodified** straight out of the
// CPM download. That is the point of this file: no upstream source is copied
// into this repository, and the only substitutions are the FFT, the bounds fix
// in parabolic interpolation, and the HMM stub -- all in files beside this one.
//
// The probabilistic estimators (pyin, pmpm) are consequently non-functional and
// must not be called; see HmmStub.cpp and HACKING.md.

#include <algorithm>
#include <complex>
#include <cstddef>
#include <utility>
#include <vector>

// MSVC has no POSIX <sys/types.h>, so `ssize_t` does not exist there. Upstream's
// mpm.cpp and yin.cpp declare their loop bounds with it, and they are compiled
// unmodified, so the type has to arrive from here -- this header is the only
// thing they include before using it, and the only lever we have on sources we
// deliberately do not patch. `_SSIZE_T_DEFINED` is the guard the Windows
// toolchains use, so setting it keeps a later definition from colliding.
#if defined(_MSC_VER) && !defined(_SSIZE_T_DEFINED)
#define _SSIZE_T_DEFINED
using ssize_t = std::ptrdiff_t;
#endif

namespace detail {
// Stands in for mlpack::hmm::HMM<mlpack::distribution::DiscreteDistribution>.
// Never read: it exists so upstream's probabilistic paths still name something.
struct UnsupportedHmm {};
} // namespace detail

namespace pitch_alloc {

// Buffers reused across calls for one fixed analysis window size.
class BaseAlloc {
public:
    long nfft;
    std::vector<float> out_real;
    std::vector<std::complex<float>> out_im;
    detail::UnsupportedHmm hmm;

    explicit BaseAlloc(long audio_buffer_size)
        : nfft(audio_buffer_size),
          out_real(static_cast<size_t>(nfft)),
          out_im(static_cast<size_t>(nfft) / 2 + 1) {
        clear();
    }

protected:
    void clear() {
        std::fill(out_im.begin(), out_im.end(), std::complex<float>{0.0f, 0.0f});
    }
};

// McLeod Pitch Method. Usage: pitch_alloc::Mpm<float> ma(2048);
template <typename T> class Mpm : public BaseAlloc {
public:
    explicit Mpm(long audio_buffer_size) : BaseAlloc(audio_buffer_size) {}

    T pitch(const std::vector<T>&, int);
    T probabilistic_pitch(const std::vector<T>&, int); // unsupported, returns -1
};

// YIN. Usage: pitch_alloc::Yin<float> ya(2048);
template <typename T> class Yin : public BaseAlloc {
public:
    int yin_buffer_size;
    std::vector<float> yin_buffer;

    explicit Yin(long audio_buffer_size)
        : BaseAlloc(audio_buffer_size),
          yin_buffer_size(static_cast<int>(audio_buffer_size) / 2),
          yin_buffer(static_cast<size_t>(yin_buffer_size)) {}

    T pitch(const std::vector<T>&, int);
    T probabilistic_pitch(const std::vector<T>&, int); // unsupported, returns -1
};

} // namespace pitch_alloc

// mpm() and yin() return the fundamental in Hz, or -1 when the window holds
// nothing they recognise as pitched. pmpm() and pyin() always return -1 here.
namespace pitch {

template <typename T> T mpm(const std::vector<T>&, int);
template <typename T> T yin(const std::vector<T>&, int);
template <typename T> T pmpm(const std::vector<T>&, int);
template <typename T> T pyin(const std::vector<T>&, int);

} // namespace pitch

namespace util {

template <typename T>
std::pair<T, T> parabolic_interpolation(const std::vector<float>&, int);

template <typename T>
void acorr_r(const std::vector<T>&, pitch_alloc::BaseAlloc*);

template <typename T>
T pitch_from_hmm(detail::UnsupportedHmm, const std::vector<std::pair<T, T>>&);

} // namespace util
