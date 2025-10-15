#pragma once

#include <memory>

// Forward declarations for different windowing systems
struct GLFWwindow;
struct SDL_Window;

namespace uapmd::service::gui {

struct WindowHandle {
    enum Type { GLFW, SDL2, SDL3 } type;
    union {
        GLFWwindow* glfwWindow;
        SDL_Window* sdlWindow;
    };

    explicit WindowHandle(GLFWwindow* window) : type(GLFW), glfwWindow(window) {}
    WindowHandle(SDL_Window* window, Type t) : type(t), sdlWindow(window) {}
};

class WindowingBackend {
public:
    virtual ~WindowingBackend() = default;

    virtual bool initialize() = 0;
    virtual WindowHandle* createWindow(const char* title, int width, int height) = 0;
    virtual void destroyWindow(WindowHandle* window) = 0;
    virtual bool shouldClose(WindowHandle* window) = 0;
    virtual void swapBuffers(WindowHandle* window) = 0;
    virtual void getDrawableSize(WindowHandle* window, int* width, int* height) = 0;
    virtual void shutdown() = 0;
    virtual const char* getName() const = 0;

    static std::unique_ptr<WindowingBackend> create();
};

class ImGuiPlatformBackend {
public:
    virtual ~ImGuiPlatformBackend() = default;

    virtual bool initialize(WindowHandle* window) = 0;
    virtual void processEvents() = 0;
    virtual void newFrame() = 0;
    virtual void shutdown() = 0;
    virtual const char* getName() const = 0;

    static std::unique_ptr<ImGuiPlatformBackend> create(WindowHandle* window);
};

class ImGuiRenderer {
public:
    virtual ~ImGuiRenderer() = default;

    virtual bool initialize(WindowHandle* window) = 0;
    virtual void newFrame() = 0;
    virtual void renderDrawData() = 0;
    virtual void shutdown() = 0;
    virtual const char* getName() const = 0;

    static std::unique_ptr<ImGuiRenderer> create();
};

} // namespace uapmd::service::gui
