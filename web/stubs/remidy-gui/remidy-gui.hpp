#pragma once

#include <functional>
#include <memory>

namespace remidy {

namespace gui {

struct Bounds { int x{0}; int y{0}; int width{0}; int height{0}; };

class ContainerWindow {
public:
    virtual ~ContainerWindow() = default;
    virtual void show(bool visible) = 0;
    virtual void resize(int w, int h) = 0;
    virtual void setResizeCallback(std::function<void(int,int)> cb) = 0;
    virtual void setResizable(bool resizable) = 0;
    virtual Bounds getBounds() const = 0;
    virtual void* getHandle() const = 0;

    static std::unique_ptr<ContainerWindow> create(const char* title, int w, int h, std::function<void()> onClose);
};

// Minimal GL context guard used by ImGuiEventLoop; no-op on web stub
struct GLContextGuard {
    GLContextGuard() = default;
    ~GLContextGuard() = default;
};

} // namespace gui

} // namespace remidy
