#include "PlatformBackend.hpp"
#include <iostream>
#include <stdexcept>

#ifdef __APPLE__
#include <choc/platform/choc_ObjectiveCHelpers.h>
#endif

// Include headers conditionally based on compile-time definitions
#ifdef USE_SDL3_BACKEND
    #include <SDL3/SDL.h>
    #include <imgui_impl_sdl3.h>
#endif

#ifdef USE_SDL2_BACKEND
    #include <SDL.h>
    #include <imgui_impl_sdl2.h>
#endif

#ifdef USE_GLFW_BACKEND
    #include <GLFW/glfw3.h>
    #include <imgui_impl_glfw.h>
#endif

#include <imgui_impl_opengl3.h>
#include <imgui.h>

namespace uapmd {
namespace gui {

// =============================================================================
// SDL3 Backend Implementation
// =============================================================================
#ifdef USE_SDL3_BACKEND

// Global quit flag for SDL3
static bool g_SDL3_QuitRequested = false;

class SDL3WindowingBackend : public WindowingBackend {
private:
    bool initialized = false;
    WindowHandle* currentWindow = nullptr;
    SDL_GLContext glContext = nullptr;
#ifdef __APPLE__
    id originalView = nil;
#endif

public:
    bool initialize() override {
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            std::cerr << "SDL3 Error: " << SDL_GetError() << std::endl;
            return false;
        }

        // GL 3.2 + GLSL 150 for macOS compatibility
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);

#ifdef __APPLE__
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif

        initialized = true;
        return true;
    }

    WindowHandle* createWindow(const char* title, int width, int height) override {
        if (!initialized) return nullptr;

        SDL_Window* window = SDL_CreateWindow(title, width, height,
            SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
        if (!window) {
            std::cerr << "SDL3 Window Error: " << SDL_GetError() << std::endl;
            return nullptr;
        }

        glContext = SDL_GL_CreateContext(window);
        if (!glContext) {
            std::cerr << "SDL3 GL Context Error: " << SDL_GetError() << std::endl;
            SDL_DestroyWindow(window);
            return nullptr;
        }

        SDL_GL_MakeCurrent(window, glContext);
        SDL_GL_SetSwapInterval(1); // Enable vsync

#ifdef __APPLE__
        if (originalView) {
            choc::objc::call<void>(originalView, "release");
            originalView = nil;
        }
        id nsContext = (id)glContext;
        if (nsContext) {
            id currentView = choc::objc::call<id>(nsContext, "view");
            if (currentView) {
                originalView = choc::objc::call<id>(currentView, "retain");
            }
        }
#endif

        currentWindow = new WindowHandle(window, WindowHandle::SDL3);
        return currentWindow;
    }

    void destroyWindow(WindowHandle* window) override {
        if (window && window->type == WindowHandle::SDL3) {
            SDL_DestroyWindow(window->sdlWindow);
            delete window;
            if (window == currentWindow) currentWindow = nullptr;
        }
#ifdef __APPLE__
        if (originalView) {
            choc::objc::call<void>(originalView, "release");
            originalView = nil;
        }
#endif
    }

    bool shouldClose(WindowHandle* window) override {
        return g_SDL3_QuitRequested || window == nullptr || window->sdlWindow == nullptr;
    }

    void swapBuffers(WindowHandle* window) override {
        if (window && window->type == WindowHandle::SDL3) {
            SDL_GL_SwapWindow(window->sdlWindow);
        }
    }

    void getDrawableSize(WindowHandle* window, int* width, int* height) override {
        if (window && window->type == WindowHandle::SDL3) {
            SDL_GetWindowSizeInPixels(window->sdlWindow, width, height);
        }
    }

    void setWindowSize(WindowHandle* window, int width, int height) override {
        if (window && window->type == WindowHandle::SDL3) {
            SDL_SetWindowSize(window->sdlWindow, width, height);
        }
    }

    void makeContextCurrent(WindowHandle* window) override {
        if (window && window->type == WindowHandle::SDL3 && glContext) {
            SDL_GL_MakeCurrent(window->sdlWindow, glContext);
#ifdef __APPLE__
            id nsContext = (id)glContext;
            if (nsContext) {
                choc::objc::call<void>(nsContext, "makeCurrentContext");
                if (originalView) {
                    choc::objc::call<void>(nsContext, "setView:", originalView);
                    choc::objc::call<void>(nsContext, "update");
                }
            }
#endif
        }
    }

    void shutdown() override {
        if (initialized) {
            if (glContext) {
                SDL_GL_DestroyContext(glContext);
                glContext = nullptr;
            }
            SDL_Quit();
            initialized = false;
        }
#ifdef __APPLE__
        if (originalView) {
            choc::objc::call<void>(originalView, "release");
            originalView = nil;
        }
#endif
    }

    const char* getName() const override {
        return "SDL3";
    }
};

class SDL3ImGuiBackend : public ImGuiPlatformBackend {
private:
    WindowHandle* window = nullptr;

public:
    bool initialize(WindowHandle* win) override {
        if (!win || win->type != WindowHandle::SDL3) return false;
        window = win;
        return ImGui_ImplSDL3_InitForOpenGL(win->sdlWindow, SDL_GL_GetCurrentContext());
    }

    void processEvents() override {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            // Treat window close requests as quit for single-window app
            if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                g_SDL3_QuitRequested = true;
            }
        }
    }

    void newFrame() override {
        ImGui_ImplSDL3_NewFrame();
    }

    void shutdown() override {
        ImGui_ImplSDL3_Shutdown();
    }

    const char* getName() const override {
        return "SDL3 ImGui Backend";
    }
};

#endif // USE_SDL3_BACKEND

// =============================================================================
// SDL2 Backend Implementation
// =============================================================================
#ifdef USE_SDL2_BACKEND

// Global quit flag for SDL2
static bool g_SDL2_QuitRequested = false;

class SDL2WindowingBackend : public WindowingBackend {
private:
    bool initialized = false;
    WindowHandle* currentWindow = nullptr;
    SDL_GLContext glContext = nullptr;
#ifdef __APPLE__
    id originalView = nil;
#endif

public:
    bool initialize() override {
        if (SDL_Init(SDL_INIT_VIDEO) < 0) {
            std::cerr << "SDL2 Error: " << SDL_GetError() << std::endl;
            return false;
        }

        // GL 3.2 + GLSL 150 for macOS compatibility
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);

#ifdef __APPLE__
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif

        initialized = true;
        return true;
    }

    WindowHandle* createWindow(const char* title, int width, int height) override {
        if (!initialized) return nullptr;

        SDL_Window* window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            width, height, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
        if (!window) {
            std::cerr << "SDL2 Window Error: " << SDL_GetError() << std::endl;
            return nullptr;
        }

        glContext = SDL_GL_CreateContext(window);
        if (!glContext) {
            std::cerr << "SDL2 GL Context Error: " << SDL_GetError() << std::endl;
            SDL_DestroyWindow(window);
            return nullptr;
        }

        SDL_GL_MakeCurrent(window, glContext);
        SDL_GL_SetSwapInterval(1); // Enable vsync

#ifdef __APPLE__
        if (originalView) {
            releaseObject(originalView);
            originalView = nil;
        }
        id nsContext = (id)glContext;
        if (nsContext) {
            SEL viewSel = sel_registerName("view");
            id currentView = objc_send_id(nsContext, viewSel);
            if (currentView) {
                originalView = retainObject(currentView);
            }
        }
#endif

        currentWindow = new WindowHandle(window, WindowHandle::SDL2);
        return currentWindow;
    }

    void destroyWindow(WindowHandle* window) override {
        if (window && window->type == WindowHandle::SDL2) {
            SDL_DestroyWindow(window->sdlWindow);
            delete window;
            if (window == currentWindow) currentWindow = nullptr;
        }
#ifdef __APPLE__
        if (originalView) {
            releaseObject(originalView);
            originalView = nil;
        }
#endif
    }

    bool shouldClose(WindowHandle* window) override {
        return g_SDL2_QuitRequested || window == nullptr || window->sdlWindow == nullptr;
    }

    void swapBuffers(WindowHandle* window) override {
        if (window && window->type == WindowHandle::SDL2) {
            SDL_GL_SwapWindow(window->sdlWindow);
        }
    }

    void getDrawableSize(WindowHandle* window, int* width, int* height) override {
        if (window && window->type == WindowHandle::SDL2) {
            SDL_GL_GetDrawableSize(window->sdlWindow, width, height);
        }
    }

    void setWindowSize(WindowHandle* window, int width, int height) override {
        if (window && window->type == WindowHandle::SDL2) {
            SDL_SetWindowSize(window->sdlWindow, width, height);
        }
    }

    void makeContextCurrent(WindowHandle* window) override {
        if (window && window->type == WindowHandle::SDL2 && glContext) {
            SDL_GL_MakeCurrent(window->sdlWindow, glContext);
#ifdef __APPLE__
            id nsContext = (id)glContext;
            if (nsContext) {
                SEL makeCurrentSel = sel_registerName("makeCurrentContext");
                objc_send_void(nsContext, makeCurrentSel);
                if (originalView) {
                    SEL setViewSel = sel_registerName("setView:");
                    objc_send_void_id(nsContext, setViewSel, originalView);
                    SEL updateSel = sel_registerName("update");
                    objc_send_void(nsContext, updateSel);
                }
            }
#endif
        }
    }

    void shutdown() override {
        if (initialized) {
            if (glContext) {
                SDL_GL_DeleteContext(glContext);
                glContext = nullptr;
            }
            SDL_Quit();
            initialized = false;
        }
#ifdef __APPLE__
        if (originalView) {
            releaseObject(originalView);
            originalView = nil;
        }
#endif
    }

    const char* getName() const override {
        return "SDL2";
    }
};

class SDL2ImGuiBackend : public ImGuiPlatformBackend {
private:
    WindowHandle* window = nullptr;

public:
    bool initialize(WindowHandle* win) override {
        if (!win || win->type != WindowHandle::SDL2) return false;
        window = win;
        return ImGui_ImplSDL2_InitForOpenGL(win->sdlWindow, SDL_GL_GetCurrentContext());
    }

    void processEvents() override {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            // Treat window close requests as quit for single-window app
            if (event.type == SDL_QUIT) {
                g_SDL2_QuitRequested = true;
            } else if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE) {
                g_SDL2_QuitRequested = true;
            }
        }
    }

    void newFrame() override {
        ImGui_ImplSDL2_NewFrame();
    }

    void shutdown() override {
        ImGui_ImplSDL2_Shutdown();
    }

    const char* getName() const override {
        return "SDL2 ImGui Backend";
    }
};

#endif // USE_SDL2_BACKEND

// =============================================================================
// GLFW Backend Implementation
// =============================================================================
#ifdef USE_GLFW_BACKEND

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

#endif // USE_GLFW_BACKEND

// =============================================================================
// OpenGL3 Renderer Implementation
// =============================================================================

class OpenGL3Renderer : public ImGuiRenderer {
public:
    bool initialize(WindowHandle* window) override {
        return ImGui_ImplOpenGL3_Init("#version 150");
    }

    void newFrame() override {
        ImGui_ImplOpenGL3_NewFrame();
    }

    void renderDrawData() override {
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    }

    void shutdown() override {
        ImGui_ImplOpenGL3_Shutdown();
    }

    const char* getName() const override {
        return "OpenGL3";
    }
};

// =============================================================================
// Factory Methods
// =============================================================================

std::unique_ptr<WindowingBackend> WindowingBackend::create() {
#ifdef USE_SDL3_BACKEND
    return std::make_unique<SDL3WindowingBackend>();
#elif defined(USE_SDL2_BACKEND)
    return std::make_unique<SDL2WindowingBackend>();
#elif defined(USE_GLFW_BACKEND)
    return std::make_unique<GLFWWindowingBackend>();
#else
    throw std::runtime_error("No windowing backend available");
#endif
}

std::unique_ptr<ImGuiPlatformBackend> ImGuiPlatformBackend::create(WindowHandle* window) {
    if (!window) return nullptr;

    switch (window->type) {
#ifdef USE_SDL3_BACKEND
        case WindowHandle::SDL3:
            return std::make_unique<SDL3ImGuiBackend>();
#endif
#ifdef USE_SDL2_BACKEND
        case WindowHandle::SDL2:
            return std::make_unique<SDL2ImGuiBackend>();
#endif
#ifdef USE_GLFW_BACKEND
        case WindowHandle::GLFW:
            return std::make_unique<GLFWImGuiBackend>();
#endif
        default:
            return nullptr;
    }
}

std::unique_ptr<ImGuiRenderer> ImGuiRenderer::create() {
    return std::make_unique<OpenGL3Renderer>();
}

} // namespace gui
} // namespace uapmd
