#pragma once

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace uapmd_app_gui {

// Whether a press held in place opens the context menus the desktop reaches by right-click.
// Only on touch platforms, which have no right button: on the desktop a held press is how an
// ImGui drag starts (on macOS in particular), so a long press there would pop a menu over the
// drag it was meant to be.
#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
constexpr bool kLongPressOpensContextMenu = true;
#else
constexpr bool kLongPressOpensContextMenu = false;
#endif

} // namespace uapmd_app_gui
