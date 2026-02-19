#pragma once

#if __has_include(<IconsFontaudio.h>)
#include <IconsFontaudio.h>
#else
#error "IconsFontaudio.h not found; ensure IconFontCppHeaders is available."
#endif

namespace uapmd::gui::icons {

namespace detail {
constexpr const char* toUtf8(const char* value) {
    return value;
}
constexpr const char* toUtf8(const char8_t* value) {
    return reinterpret_cast<const char*>(value);
}
} // namespace detail

inline const char* kDeleteTrack = detail::toUtf8(ICON_FAD_ERASER);

} // namespace uapmd::gui::icons
