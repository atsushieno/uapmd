#if defined(__EMSCRIPTEN__)

#include <remidy-gui/remidy-gui.hpp>
#include <iostream>
#include <string>

namespace remidy::gui {

class EmscriptenContainerWindow : public ContainerWindow {
public:
    EmscriptenContainerWindow(const char* title, int width, int height, std::function<void()> closeCallback)
        : closeCallback_(std::move(closeCallback)) {
        bounds_.width = width > 0 ? width : bounds_.width;
        bounds_.height = height > 0 ? height : bounds_.height;
        if (title) {
            title_ = title;
        }
    }

    void show(bool visible) override {
        visible_ = visible;
        if (!visible && closeCallback_) {
            closeCallback_();
        }
    }

    void resize(int width, int height) override {
        if (width > 0) {
            bounds_.width = width;
        }
        if (height > 0) {
            bounds_.height = height;
        }
        if (resizeCallback_) {
            resizeCallback_(bounds_.width, bounds_.height);
        }
    }

    void setResizeCallback(std::function<void(int, int)> callback) override {
        resizeCallback_ = std::move(callback);
    }

    void setResizable(bool) override {
        // No-op: browser canvas sizing is controlled by the host page
    }

    Bounds getBounds() const override {
        return bounds_;
    }

    void* getHandle() const override {
        return nullptr;
    }

private:
    std::function<void()> closeCallback_;
    std::function<void(int, int)> resizeCallback_;
    Bounds bounds_{};
    std::string title_;
    bool visible_{false};
};

std::unique_ptr<ContainerWindow> ContainerWindow::create(const char* title,
                                                         int width,
                                                         int height,
                                                         std::function<void()> closeCallback) {
    std::cout << "[uapmd] Plugin UI windows are unavailable on WebAssembly builds; creating stub container for '"
              << (title ? title : "plugin")
              << "'." << std::endl;
    return std::make_unique<EmscriptenContainerWindow>(title, width, height, std::move(closeCallback));
}

} // namespace remidy::gui

#endif // defined(__EMSCRIPTEN__)
