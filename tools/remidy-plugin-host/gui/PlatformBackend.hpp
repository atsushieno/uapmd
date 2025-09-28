#pragma once

#include <memory>
#include <string>

// Forward declarations for different windowing systems
struct GLFWwindow;
struct SDL_Window;

namespace uapmd {
namespace gui {

/**
 * Window handle that can contain different types of windows
 */
struct WindowHandle {
    enum Type { GLFW, SDL2, SDL3 } type;
    union {
        GLFWwindow* glfwWindow;
        SDL_Window* sdlWindow;
    };

    WindowHandle(GLFWwindow* window) : type(GLFW), glfwWindow(window) {}
    WindowHandle(SDL_Window* window, Type t) : type(t), sdlWindow(window) {}
};

/**
 * Abstract interface for windowing system backends
 * Supports SDL3, SDL2, and GLFW with automatic selection
 */
class WindowingBackend {
public:
    virtual ~WindowingBackend() = default;

    /**
     * Initialize the windowing system
     * @return true if successful
     */
    virtual bool initialize() = 0;

    /**
     * Create a window
     * @param title Window title
     * @param width Window width
     * @param height Window height
     * @return Window handle or nullptr on failure
     */
    virtual WindowHandle* createWindow(const char* title, int width, int height) = 0;

    /**
     * Destroy a window
     */
    virtual void destroyWindow(WindowHandle* window) = 0;

    /**
     * Check if the window should close (e.g., user clicked close button)
     */
    virtual bool shouldClose(WindowHandle* window) = 0;

    /**
     * Swap buffers for the window
     */
    virtual void swapBuffers(WindowHandle* window) = 0;

    /**
     * Get window drawable size
     */
    virtual void getDrawableSize(WindowHandle* window, int* width, int* height) = 0;

    /**
     * Shutdown the windowing system
     */
    virtual void shutdown() = 0;

    /**
     * Get the name of this backend
     */
    virtual const char* getName() const = 0;

    /**
     * Create windowing backend instance
     * Automatically selects based on available libraries
     */
    static std::unique_ptr<WindowingBackend> create();
};

/**
 * Abstract interface for ImGui platform backends
 */
class ImGuiPlatformBackend {
public:
    virtual ~ImGuiPlatformBackend() = default;

    /**
     * Initialize the ImGui platform backend
     * @param window Window handle
     * @return true if successful
     */
    virtual bool initialize(WindowHandle* window) = 0;

    /**
     * Process events for ImGui
     */
    virtual void processEvents() = 0;

    /**
     * Start a new ImGui frame
     */
    virtual void newFrame() = 0;

    /**
     * Shutdown the platform backend
     */
    virtual void shutdown() = 0;

    /**
     * Get the name of this backend
     */
    virtual const char* getName() const = 0;

    /**
     * Create ImGui platform backend instance
     * Based on the windowing backend type
     */
    static std::unique_ptr<ImGuiPlatformBackend> create(WindowHandle* window);
};

/**
 * Abstract interface for ImGui renderers
 */
class ImGuiRenderer {
public:
    virtual ~ImGuiRenderer() = default;

    /**
     * Initialize the renderer
     * @param window Window handle
     * @return true if successful
     */
    virtual bool initialize(WindowHandle* window) = 0;

    /**
     * Start a new frame
     */
    virtual void newFrame() = 0;

    /**
     * Render the frame
     */
    virtual void renderDrawData() = 0;

    /**
     * Shutdown the renderer
     */
    virtual void shutdown() = 0;

    /**
     * Get the name of this renderer
     */
    virtual const char* getName() const = 0;

    /**
     * Create renderer instance
     * Automatically selects OpenGL3 (most compatible)
     */
    static std::unique_ptr<ImGuiRenderer> create();
};

} // namespace gui
} // namespace uapmd