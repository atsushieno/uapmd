#if defined(__EMSCRIPTEN__)

#include <remidy-gui/remidy-gui.hpp>
#include <emscripten.h>
#include <iostream>
#include <string>

namespace remidy::gui {

extern "C" {
EMSCRIPTEN_KEEPALIVE void remidy_emscripten_container_window_notify_close(uintptr_t handle);
EMSCRIPTEN_KEEPALIVE void remidy_emscripten_container_window_notify_resize(uintptr_t handle, int width, int height);
}

EM_JS(void, remidy_emscripten_container_window_create_js,
      (uintptr_t handle, const char* title, const char* contentId, int width, int height), {
    if (!Module._remidyContainerWindows) {
        Module._remidyContainerWindows = new Map();
    }
    if (!Module._remidyContainerWindowsZ) {
        Module._remidyContainerWindowsZ = 2000;
    }
    if (Module._remidyActiveContainerWindow === undefined) {
        Module._remidyActiveContainerWindow = 0;
    }
    const titleText = UTF8ToString(title || 0);
    const bodyId = UTF8ToString(contentId || 0);
    const rootWidth = Math.max(width, 240);
    const rootHeight = Math.max(height + 36, 180);
    const left = Math.max(24, Math.round((window.innerWidth - rootWidth) / 2));
    const top = Math.max(24, Math.round((window.innerHeight - rootHeight) / 3));

    const root = document.createElement('div');
    root.id = `remidy-container-${handle}`;
    root.style.position = 'fixed';
    root.style.left = `${left}px`;
    root.style.top = `${top}px`;
    root.style.width = `${rootWidth}px`;
    root.style.height = `${rootHeight}px`;
    root.style.display = 'none';
    root.style.flexDirection = 'column';
    root.style.background = '#171720';
    root.style.border = '1px solid #8c53c7';
    root.style.boxShadow = '0 18px 48px rgba(0,0,0,0.45)';
    root.style.zIndex = String(++Module._remidyContainerWindowsZ);
    root.style.resize = 'none';
    root.style.overflow = 'hidden';
    root.style.minWidth = '240px';
    root.style.minHeight = '180px';
    root.dataset.remidyWindow = String(handle);

    const header = document.createElement('div');
    header.style.height = '36px';
    header.style.flex = '0 0 36px';
    header.style.display = 'flex';
    header.style.alignItems = 'center';
    header.style.justifyContent = 'space-between';
    header.style.padding = '0 10px';
    header.style.background = '#8c53c7';
    header.style.color = '#fff';
    header.style.font = '600 14px system-ui, sans-serif';
    header.style.cursor = 'move';
    header.style.userSelect = 'none';

    const titleSpan = document.createElement('span');
    titleSpan.textContent = titleText || 'Plugin UI';
    titleSpan.style.overflow = 'hidden';
    titleSpan.style.textOverflow = 'ellipsis';
    titleSpan.style.whiteSpace = 'nowrap';

    const closeButton = document.createElement('button');
    closeButton.type = 'button';
    closeButton.textContent = 'x';
    closeButton.style.border = '0';
    closeButton.style.background = 'transparent';
    closeButton.style.color = '#fff';
    closeButton.style.cursor = 'pointer';
    closeButton.style.font = '600 16px system-ui, sans-serif';
    closeButton.onclick = () => {
        _remidy_emscripten_container_window_notify_close(handle);
    };

    header.appendChild(titleSpan);
    header.appendChild(closeButton);

    const body = document.createElement('div');
    body.id = bodyId;
    body.style.flex = '1 1 auto';
    body.style.position = 'relative';
    body.style.width = '100%';
    body.style.height = `${Math.max(height, 120)}px`;
    body.style.background = '#0f0f16';
    body.style.overflow = 'hidden';

    const setActive = (active) => {
        root.dataset.active = active ? 'true' : 'false';
        root.style.zIndex = String(active ? ++Module._remidyContainerWindowsZ : root.style.zIndex);
        root.style.pointerEvents = active ? 'auto' : 'none';
        header.style.pointerEvents = 'auto';
        body.style.pointerEvents = active ? 'auto' : 'none';
        body.style.opacity = active ? '1' : '0.92';
        const iframe = body.querySelector('iframe');
        if (iframe)
            iframe.style.pointerEvents = active ? 'auto' : 'none';
        if (active)
            Module._remidyActiveContainerWindow = handle;
        else if (Module._remidyActiveContainerWindow === handle)
            Module._remidyActiveContainerWindow = 0;
    };

    root.appendChild(header);
    root.appendChild(body);
    document.body.appendChild(root);

    const bringToFront = () => {
        if (Module._remidyActiveContainerWindow &&
            Module._remidyActiveContainerWindow !== handle) {
            const prev = Module._remidyContainerWindows.get(Module._remidyActiveContainerWindow);
            if (prev)
                prev.setActive(false);
        }
        setActive(true);
    };

    let dragState = null;
    const onPointerMove = (event) => {
        if (!dragState)
            return;
        const nextLeft = dragState.startLeft + event.clientX - dragState.startX;
        const nextTop = dragState.startTop + event.clientY - dragState.startY;
        root.style.left = `${nextLeft}px`;
        root.style.top = `${nextTop}px`;
    };
    const endDrag = () => {
        if (!dragState)
            return;
        try {
            header.releasePointerCapture(dragState.pointerId);
        } catch (_) {}
        dragState = null;
    };
    header.addEventListener('pointerdown', (event) => {
        if (event.target === closeButton || event.button !== 0)
            return;
        bringToFront();
        dragState = {
            pointerId: event.pointerId,
            startX: event.clientX,
            startY: event.clientY,
            startLeft: root.offsetLeft,
            startTop: root.offsetTop,
        };
        header.setPointerCapture(event.pointerId);
        event.preventDefault();
    });
    header.addEventListener('pointermove', onPointerMove);
    header.addEventListener('pointerup', endDrag);
    header.addEventListener('pointercancel', endDrag);

    const notifyResize = () => {
        const rect = body.getBoundingClientRect();
        _remidy_emscripten_container_window_notify_resize(handle,
            Math.max(1, Math.round(rect.width)),
            Math.max(1, Math.round(rect.height)));
    };
    const resizeObserver = new ResizeObserver(() => notifyResize());
    resizeObserver.observe(root);

    Module._remidyContainerWindows.set(handle, {
        root,
        body,
        header,
        titleSpan,
        resizeObserver,
        setActive,
    });
    setActive(false);
});

EM_JS(void, remidy_emscripten_container_window_destroy_js, (uintptr_t handle), {
    const state = Module._remidyContainerWindows && Module._remidyContainerWindows.get(handle);
    if (!state)
        return;
    state.resizeObserver.disconnect();
    state.root.remove();
    Module._remidyContainerWindows.delete(handle);
});

EM_JS(void, remidy_emscripten_container_window_show_js, (uintptr_t handle, int visible), {
    const state = Module._remidyContainerWindows && Module._remidyContainerWindows.get(handle);
    if (!state)
        return;
    state.root.style.display = visible ? 'flex' : 'none';
    if (visible)
        state.setActive(true);
    else
        state.setActive(false);
});

EM_JS(void, remidy_emscripten_container_window_resize_js, (uintptr_t handle, int width, int height), {
    const state = Module._remidyContainerWindows && Module._remidyContainerWindows.get(handle);
    if (!state)
        return;
    state.root.style.width = `${Math.max(width, 240)}px`;
    state.root.style.height = `${Math.max(height + 36, 180)}px`;
    state.body.style.height = `${Math.max(height, 120)}px`;
});

EM_JS(void, remidy_emscripten_container_window_set_title_js, (uintptr_t handle, const char* title), {
    const state = Module._remidyContainerWindows && Module._remidyContainerWindows.get(handle);
    if (!state)
        return;
    state.titleSpan.textContent = UTF8ToString(title || 0);
});

EM_JS(void, remidy_emscripten_container_window_set_resizable_js, (uintptr_t handle, int resizable), {
    const state = Module._remidyContainerWindows && Module._remidyContainerWindows.get(handle);
    if (!state)
        return;
    state.root.style.resize = resizable ? 'both' : 'none';
    state.root.style.overflow = resizable ? 'auto' : 'hidden';
});

class EmscriptenContainerWindow : public ContainerWindow {
public:
    EmscriptenContainerWindow(const char* title, int width, int height, std::function<void()> closeCallback)
        : closeCallback_(std::move(closeCallback)) {
        bounds_.width = width > 0 ? width : bounds_.width;
        bounds_.height = height > 0 ? height : bounds_.height;
        if (title) {
            title_ = title;
        }
        content_id_ = "remidy-container-body-" + std::to_string(reinterpret_cast<uintptr_t>(this));
        remidy_emscripten_container_window_create_js(reinterpret_cast<uintptr_t>(this),
                                                     title_.c_str(),
                                                     content_id_.c_str(),
                                                     bounds_.width,
                                                     bounds_.height);
    }

    ~EmscriptenContainerWindow() override {
        remidy_emscripten_container_window_destroy_js(reinterpret_cast<uintptr_t>(this));
    }

    void show(bool visible) override {
        visible_ = visible;
        remidy_emscripten_container_window_show_js(reinterpret_cast<uintptr_t>(this), visible ? 1 : 0);
    }

    void resize(int width, int height) override {
        const auto next_width = width > 0 ? width : bounds_.width;
        const auto next_height = height > 0 ? height : bounds_.height;
        if (next_width == bounds_.width && next_height == bounds_.height)
            return;
        if (width > 0) {
            bounds_.width = width;
        }
        if (height > 0) {
            bounds_.height = height;
        }
        remidy_emscripten_container_window_resize_js(reinterpret_cast<uintptr_t>(this),
                                                     bounds_.width,
                                                     bounds_.height);
        if (resizeCallback_) {
            resizeCallback_(bounds_.width, bounds_.height);
        }
    }

    void setResizeCallback(std::function<void(int, int)> callback) override {
        resizeCallback_ = std::move(callback);
    }

    void setResizable(bool resizable) override {
        resizable_ = resizable;
        remidy_emscripten_container_window_set_resizable_js(reinterpret_cast<uintptr_t>(this),
                                                            resizable ? 1 : 0);
    }

    Bounds getBounds() const override {
        return bounds_;
    }

    void* getHandle() const override {
        return const_cast<char*>(content_id_.c_str());
    }

    void setTitle(const std::string& title) {
        title_ = title;
        remidy_emscripten_container_window_set_title_js(reinterpret_cast<uintptr_t>(this), title_.c_str());
    }

    void notifyClose() {
        if (closeCallback_) {
            closeCallback_();
        }
    }

    void notifyResize(int width, int height) {
        const auto next_width = width > 0 ? width : bounds_.width;
        const auto next_height = height > 0 ? height : bounds_.height;
        if (next_width == bounds_.width && next_height == bounds_.height)
            return;
        bounds_.width = next_width;
        bounds_.height = next_height;
        if (resizeCallback_) {
            resizeCallback_(bounds_.width, bounds_.height);
        }
    }

private:
    std::function<void()> closeCallback_;
    std::function<void(int, int)> resizeCallback_;
    Bounds bounds_{};
    std::string title_;
    std::string content_id_;
    bool visible_{false};
    bool resizable_{false};
};

std::unique_ptr<ContainerWindow> ContainerWindow::create(const char* title,
                                                         int width,
                                                         int height,
                                                         std::function<void()> closeCallback) {
    return std::make_unique<EmscriptenContainerWindow>(title, width, height, std::move(closeCallback));
}

extern "C" EMSCRIPTEN_KEEPALIVE
void remidy_emscripten_container_window_notify_close(uintptr_t handle) {
    auto* window = reinterpret_cast<EmscriptenContainerWindow*>(handle);
    if (window)
        window->notifyClose();
}

extern "C" EMSCRIPTEN_KEEPALIVE
void remidy_emscripten_container_window_notify_resize(uintptr_t handle, int width, int height) {
    auto* window = reinterpret_cast<EmscriptenContainerWindow*>(handle);
    if (window)
        window->notifyResize(width, height);
}

} // namespace remidy::gui

#endif // defined(__EMSCRIPTEN__)
