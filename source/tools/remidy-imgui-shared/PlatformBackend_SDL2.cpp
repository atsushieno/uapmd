#include "PlatformBackend.hpp"

#ifdef USE_SDL2_BACKEND

#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <iostream>
#include <SDL.h>

#ifdef __APPLE__
#include <choc/platform/choc_ObjectiveCHelpers.h>
#endif

namespace uapmd {
namespace gui {

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

std::unique_ptr<WindowingBackend> createSDL2WindowingBackend() {
    return std::make_unique<SDL2WindowingBackend>();
}

std::unique_ptr<ImGuiPlatformBackend> createSDL2ImGuiBackend() {
    return std::make_unique<SDL2ImGuiBackend>();
}

} // namespace gui
} // namespace uapmd

#endif // USE_SDL2_BACKEND
