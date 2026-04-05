/* uapmd C API — implementation for remidy-gui bindings */

#include "c-api/uapmd-c-gui.h"
#include <remidy-gui/remidy-gui.hpp>
#include <unordered_map>
#include <mutex>
#include <memory>
#include <functional>

/* ── Cast helpers ─────────────────────────────────────────────────────────── */

static remidy::gui::ContainerWindow* CW(uapmd_container_window_t h) { return reinterpret_cast<remidy::gui::ContainerWindow*>(h); }

/* ── Ownership registry ───────────────────────────────────────────────────── */

static std::mutex s_window_mutex;
static std::unordered_map<remidy::gui::ContainerWindow*, std::unique_ptr<remidy::gui::ContainerWindow>> s_owned_windows;

/* We need to store the C callback + user_data so the C++ std::function can invoke it. */
struct WindowCallbacks {
    uapmd_window_close_cb_t close_cb{};
    void* close_ud{};
    uapmd_window_resize_cb_t resize_cb{};
    void* resize_ud{};
};
static std::mutex s_cb_mutex;
static std::unordered_map<remidy::gui::ContainerWindow*, WindowCallbacks> s_window_cbs;

/* ═══════════════════════════════════════════════════════════════════════════
 *  ContainerWindow
 * ═══════════════════════════════════════════════════════════════════════════ */

uapmd_container_window_t uapmd_container_window_create(const char* title,
                                                        int width, int height,
                                                        void* close_user_data,
                                                        uapmd_window_close_cb_t close_callback) {
    auto closeCb = [close_callback, close_user_data]() {
        if (close_callback)
            close_callback(close_user_data);
    };
    auto win = remidy::gui::ContainerWindow::create(title ? title : "", width, height, closeCb);
    auto raw = win.get();
    {
        std::lock_guard lock(s_window_mutex);
        s_owned_windows[raw] = std::move(win);
    }
    {
        std::lock_guard lock(s_cb_mutex);
        s_window_cbs[raw] = { close_callback, close_user_data, nullptr, nullptr };
    }
    return reinterpret_cast<uapmd_container_window_t>(raw);
}

void uapmd_container_window_destroy(uapmd_container_window_t win) {
    if (!win) return;
    auto* raw = CW(win);
    {
        std::lock_guard lock(s_cb_mutex);
        s_window_cbs.erase(raw);
    }
    {
        std::lock_guard lock(s_window_mutex);
        s_owned_windows.erase(raw);
    }
}

void uapmd_container_window_show(uapmd_container_window_t win, bool visible) {
    CW(win)->show(visible);
}

void uapmd_container_window_resize(uapmd_container_window_t win, int width, int height) {
    CW(win)->resize(width, height);
}

void uapmd_container_window_set_resize_callback(uapmd_container_window_t win,
                                                  void* user_data,
                                                  uapmd_window_resize_cb_t callback) {
    auto* raw = CW(win);
    {
        std::lock_guard lock(s_cb_mutex);
        s_window_cbs[raw].resize_cb = callback;
        s_window_cbs[raw].resize_ud = user_data;
    }
    raw->setResizeCallback([callback, user_data](int w, int h) {
        if (callback)
            callback(w, h, user_data);
    });
}

void uapmd_container_window_set_resizable(uapmd_container_window_t win, bool resizable) {
    CW(win)->setResizable(resizable);
}

uapmd_bounds_t uapmd_container_window_get_bounds(uapmd_container_window_t win) {
    auto b = CW(win)->getBounds();
    return { b.x, b.y, b.width, b.height };
}

void* uapmd_container_window_get_handle(uapmd_container_window_t win) {
    return CW(win)->getHandle();
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  GLContextGuard
 * ═══════════════════════════════════════════════════════════════════════════ */

uapmd_gl_context_guard_t uapmd_gl_context_guard_create() {
    auto guard = new remidy::gui::GLContextGuard();
    return reinterpret_cast<uapmd_gl_context_guard_t>(guard);
}

void uapmd_gl_context_guard_destroy(uapmd_gl_context_guard_t guard) {
    if (!guard) return;
    delete reinterpret_cast<remidy::gui::GLContextGuard*>(guard);
}
