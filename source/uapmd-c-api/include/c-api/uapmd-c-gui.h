/* uapmd C API — bindings for remidy-gui (ContainerWindow, GLContextGuard) */
#ifndef UAPMD_C_GUI_H
#define UAPMD_C_GUI_H

#include "uapmd-c-common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Opaque handles ─────────────────────────────────────────────────────── */

typedef struct uapmd_container_window*   uapmd_container_window_t;
typedef struct uapmd_gl_context_guard*   uapmd_gl_context_guard_t;

/* ── Data structs ───────────────────────────────────────────────────────── */

typedef struct uapmd_bounds {
    int x;
    int y;
    int width;
    int height;
} uapmd_bounds_t;

/* ── Callbacks ──────────────────────────────────────────────────────────── */

typedef void (*uapmd_window_close_cb_t)(void* user_data);
typedef void (*uapmd_window_resize_cb_t)(int width, int height, void* user_data);

/* ═══════════════════════════════════════════════════════════════════════════
 *  ContainerWindow
 * ═══════════════════════════════════════════════════════════════════════════ */

UAPMD_C_EXPORT uapmd_container_window_t uapmd_container_window_create(const char* title,
                                                                        int width, int height,
                                                                        void* close_user_data,
                                                                        uapmd_window_close_cb_t close_callback);
UAPMD_C_EXPORT void uapmd_container_window_destroy(uapmd_container_window_t win);

UAPMD_C_EXPORT void  uapmd_container_window_show(uapmd_container_window_t win, bool visible);
UAPMD_C_EXPORT void  uapmd_container_window_resize(uapmd_container_window_t win, int width, int height);
UAPMD_C_EXPORT void  uapmd_container_window_set_resize_callback(uapmd_container_window_t win,
                                                                   void* user_data,
                                                                   uapmd_window_resize_cb_t callback);
UAPMD_C_EXPORT void  uapmd_container_window_set_resizable(uapmd_container_window_t win, bool resizable);
UAPMD_C_EXPORT uapmd_bounds_t uapmd_container_window_get_bounds(uapmd_container_window_t win);
UAPMD_C_EXPORT void* uapmd_container_window_get_handle(uapmd_container_window_t win);

/* ═══════════════════════════════════════════════════════════════════════════
 *  GLContextGuard (RAII)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Save the current GL context. Call uapmd_gl_context_guard_destroy to restore it. */
UAPMD_C_EXPORT uapmd_gl_context_guard_t uapmd_gl_context_guard_create(void);
UAPMD_C_EXPORT void uapmd_gl_context_guard_destroy(uapmd_gl_context_guard_t guard);

#ifdef __cplusplus
}
#endif

#endif /* UAPMD_C_GUI_H */
