#pragma once

#include <memory>
#include <functional>

namespace remidy::gui {

struct Bounds { int x{100}, y{100}, width{800}, height{600}; };

class ContainerWindow {
public:
    virtual ~ContainerWindow() = default;
    static std::unique_ptr<ContainerWindow> create(const char* title, int width, int height, std::function<void()> closeCallback);

    virtual void show(bool visible) = 0;
    virtual void resize(int width, int height) = 0;
    virtual Bounds getBounds() const = 0;
    virtual void* getHandle() const = 0; // Platform-specific: HWND, NSView*, XID encoded in void*
};

} // namespace remidy::gui

