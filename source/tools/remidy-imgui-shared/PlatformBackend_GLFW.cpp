#include "PlatformBackend.hpp"

#ifdef USE_GLFW_BACKEND

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <iostream>

namespace uapmd {
namespace gui {

static bool g_GLFW_QuitRequested = false;

class GLFWWindowingBackend : public WindowingBackend {
private:
    bool initialized = false;
    WindowHandle* currentWindow = nullptr;
    GLFWwindow* glContextWindow = nullptr;

public:
    bool initialize() override {
        // Prefer X11 platform when available (GLFW 3.4+)
#if defined(__linux__) && !defined(__APPLE__)
#ifdef GLFW_PLATFORM_X11
        glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
#endif
#endif
        if (!glfwInit()) {
            std::cerr << "GLFW Error: Failed to initialize" << std::endl;
            return false;
        }

        // GL 3.2 + GLSL 150 for macOS compatibility
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

#ifdef __APPLE__
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

        initialized = true;
        return true;
    }

    WindowHandle* createWindow(const char* title, int width, int height) override {
        if (!initialized) return nullptr;

        GLFWwindow* window = glfwCreateWindow(width, height, title, nullptr, nullptr);
        if (!window) {
            std::cerr << "GLFW Window Error: Failed to create window" << std::endl;
            return nullptr;
        }

        glfwMakeContextCurrent(window);
        glfwSwapInterval(1); // Enable vsync

        glContextWindow = window;
        currentWindow = new WindowHandle(window);
        return currentWindow;
    }

    void destroyWindow(WindowHandle* window) override {
        if (window && window->type == WindowHandle::GLFW) {
            glfwDestroyWindow(window->glfwWindow);
            delete window;
            if (window == currentWindow) currentWindow = nullptr;
        }
    }

    bool shouldClose(WindowHandle* window) override {
        if (!window || window->type != WindowHandle::GLFW || !window->glfwWindow) {
            return true;
        }
        return g_GLFW_QuitRequested || glfwWindowShouldClose(window->glfwWindow);
    }

    void swapBuffers(WindowHandle* window) override {
        if (window && window->type == WindowHandle::GLFW) {
            glfwSwapBuffers(window->glfwWindow);
        }
    }

    void getDrawableSize(WindowHandle* window, int* width, int* height) override {
        if (window && window->type == WindowHandle::GLFW) {
            glfwGetFramebufferSize(window->glfwWindow, width, height);
        }
    }

    void setWindowSize(WindowHandle* window, int width, int height) override {
        if (window && window->type == WindowHandle::GLFW) {
            glfwSetWindowSize(window->glfwWindow, width, height);
        }
    }

    void makeContextCurrent(WindowHandle* window) override {
        if (window && window->type == WindowHandle::GLFW && glContextWindow) {
            glfwMakeContextCurrent(glContextWindow);
        }
    }

    void shutdown() override {
        if (initialized) {
            glfwTerminate();
            initialized = false;
        }
    }

    const char* getName() const override {
        return "GLFW";
    }
};

class GLFWImGuiBackend : public ImGuiPlatformBackend {
private:
    WindowHandle* window = nullptr;

public:
    bool initialize(WindowHandle* win) override {
        if (!win || win->type != WindowHandle::GLFW) return false;
        window = win;
        bool ok = ImGui_ImplGlfw_InitForOpenGL(win->glfwWindow, true);
        if (ok) {
            // Ensure closing the window requests quit
            glfwSetWindowCloseCallback(win->glfwWindow, [](GLFWwindow* w){
                g_GLFW_QuitRequested = true;
                glfwSetWindowShouldClose(w, GLFW_TRUE);
            });
        }
        return ok;
    }

    void processEvents() override {
        glfwPollEvents();
        // GLFW automatically forwards events to ImGui when using ImGui_ImplGlfw_InitForOpenGL
    }

    void newFrame() override {
        ImGui_ImplGlfw_NewFrame();
    }

    void shutdown() override {
        ImGui_ImplGlfw_Shutdown();
    }

    const char* getName() const override {
        return "GLFW ImGui Backend";
    }
};

std::unique_ptr<WindowingBackend> createGLFWWindowingBackend() {
    return std::make_unique<GLFWWindowingBackend>();
}

std::unique_ptr<ImGuiPlatformBackend> createGLFWImGuiBackend() {
    return std::make_unique<GLFWImGuiBackend>();
}

} // namespace gui
} // namespace uapmd

#endif // USE_GLFW_BACKEND
