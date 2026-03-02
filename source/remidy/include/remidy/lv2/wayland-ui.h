#pragma once

/**
 * @file wayland-ui.h
 * LV2 Wayland UI support for remidy
 *
 * Based on the approach used in https://github.com/falkTX/wayland-audio-plugin-test
 * and discussions at https://github.com/lv2/lv2/issues/70
 *
 * This uses the de-facto standard URIs:
 * - ui:WaylandSurfaceUI (http://lv2plug.in/ns/extensions/ui#WaylandSurfaceUI) for the UI type
 * - "urn:wayland:display" for the Wayland display connection feature
 * - LV2_UI__parent for the parent wl_surface* (cast to void*)
 * - Plugin returns wl_subsurface as the widget pointer
 */

#include <lv2/ui/ui.h>

// De-facto standard URIs for Wayland support
// Used by falkTX's wayland-audio-plugin-test and other implementations
#define LV2_UI__WaylandSurfaceUI LV2_UI_PREFIX "WaylandSurfaceUI"
#define LV2_WAYLAND_DISPLAY_URI "urn:wayland:display"

#ifdef HAVE_WAYLAND
#include <wayland-client.h>
#endif // HAVE_WAYLAND
