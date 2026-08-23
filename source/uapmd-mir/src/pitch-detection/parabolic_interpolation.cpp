// Substitute for the same file in sevagh/pitch-detection (MIT, see NOTICE).
//
// Upstream's version tests `x_ > array.size() - 1`, which lets x_ == last reach
// the interpolating branch and read array[x_ + 1] past the end. YIN's
// absolute_threshold() can return exactly that index, so this rewrite tightens
// the bound. Compiled instead of upstream's copy, not alongside it.

#include "pitch_detection.h"

#include <vector>

template <typename T>
std::pair<T, T> util::parabolic_interpolation(const std::vector<float>& array, int x_) {
    const auto last = static_cast<int>(array.size()) - 1;
    if (last < 0)
        return {static_cast<T>(x_), static_cast<T>(0)};

    const auto at = [&array](int index) { return static_cast<T>(array[static_cast<size_t>(index)]); };
    const auto pick = [&at](int index) { return std::pair<T, T>{static_cast<T>(index), at(index)}; };

    // Upstream tests `x_ > array.size() - 1`, which lets x_ == last reach the
    // interpolating branch and read array[x_ + 1] past the end. YIN's
    // absolute_threshold() can return exactly that index, so the bound is
    // tightened here to keep the read in range.
    if (x_ < 1)
        return pick(x_ < last && at(x_) > at(x_ + 1) ? x_ + 1 : x_);
    if (x_ >= last)
        return pick(at(x_) > at(x_ - 1) ? x_ - 1 : x_);

    const T den = at(x_ + 1) + at(x_ - 1) - 2 * at(x_);
    const T delta = at(x_ - 1) - at(x_ + 1);
    if (!den)
        return {static_cast<T>(x_), at(x_)};
    return {static_cast<T>(x_) + delta / (2 * den),
            at(x_) - delta * delta / (8 * den)};
}

template std::pair<double, double>
util::parabolic_interpolation<double>(const std::vector<float>& array, int x);

template std::pair<float, float>
util::parabolic_interpolation<float>(const std::vector<float>& array, int x);
