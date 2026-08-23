// The probabilistic estimators that upstream backs with an mlpack HMM are not
// available here, because mlpack cannot follow this project to WebAssembly and
// Android. Upstream's mpm.cpp and yin.cpp still define pmpm() and pyin(), and
// those definitions must resolve, so this supplies the one symbol they need.
//
// Returning -1 means "no pitch", which is what every caller of the plain
// estimators already handles. Nothing in this project calls pmpm() or pyin().

#include "pitch_detection.h"

template <typename T>
T util::pitch_from_hmm(detail::UnsupportedHmm, const std::vector<std::pair<T, T>>&) {
    return -1;
}

template double util::pitch_from_hmm<double>(
    detail::UnsupportedHmm, const std::vector<std::pair<double, double>>&);
template float util::pitch_from_hmm<float>(
    detail::UnsupportedHmm, const std::vector<std::pair<float, float>>&);
