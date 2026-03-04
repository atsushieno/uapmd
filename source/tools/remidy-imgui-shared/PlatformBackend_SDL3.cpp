#include "PlatformBackend.hpp"

#ifdef USE_SDL3_BACKEND

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <iostream>
#include <SDL3/SDL.h>

#if defined(USE_SDL_RENDERER)
#include <imgui_impl_sdlrenderer3.h>
#endif

#ifdef __APPLE__
#include <choc/platform/choc_ObjectiveCHelpers.h>
#endif

#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace uapmd {
namespace gui {

// Global quit flag for SDL3
static bool g_SDL3_QuitRequested = false;

class SDL3WindowingBackend : public WindowingBackend {
private:
    bool initialized = false;
    WindowHandle* currentWindow = nullptr;
    SDL_GLContext glContext = nullptr;
    SDL_Renderer* sdlRenderer = nullptr;  // Used on iOS (SDL_Renderer → Metal)
#ifdef __APPLE__
    id originalView = nil;
#endif

public:
    bool initialize() override {
#if defined(__ANDROID__)
        // Android: Initialize SDL3 with OpenGL ES 3.0
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
            __android_log_print(ANDROID_LOG_ERROR, "UAPMD", "SDL3 Init Error: %s", SDL_GetError());
            return false;
        }

        // Configure for OpenGL ES 3.0 on Android
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);

        __android_log_print(ANDROID_LOG_INFO, "UAPMD", "SDL3 initialized for Android/GLES3");
#elif defined(USE_SDL_RENDERER)
        // iOS: Use SDL_Renderer (backed by Metal). No GL context attributes needed.
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
            SDL_Log("SDL3 Init Error: %s", SDL_GetError());
            return false;
        }
        SDL_Log("SDL3 initialized for iOS (SDL_Renderer/Metal)");
#else
        // Desktop: Initialize SDL3 with OpenGL 3.2
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
#endif

        initialized = true;
        return true;
    }

    WindowHandle* createWindow(const char* title, int width, int height) override {
        if (!initialized) return nullptr;

#if defined(USE_SDL_RENDERER)
        // iOS: Create a fullscreen Metal-capable window via SDL_Renderer.
        // SDL_WINDOW_FULLSCREEN ensures the window fills the screen and SDL reports
        // the correct logical dimensions via SDL_GetWindowSize().
        // SDL_WINDOW_HIGH_PIXEL_DENSITY ensures correct Retina resolution.
        SDL_Window* window = SDL_CreateWindow(
            title, width, height,
            SDL_WINDOW_FULLSCREEN | SDL_WINDOW_HIGH_PIXEL_DENSITY
        );
        if (!window) {
            SDL_Log("SDL3 Window Error: %s", SDL_GetError());
            return nullptr;
        }

        // SDL_CreateRenderer selects Metal automatically on iOS.
        sdlRenderer = SDL_CreateRenderer(window, nullptr);
        if (!sdlRenderer) {
            SDL_Log("SDL3 Renderer Error: %s", SDL_GetError());
            SDL_DestroyWindow(window);
            return nullptr;
        }
        SDL_SetRenderVSync(sdlRenderer, 1);
        SDL_Log("SDL3 renderer created: %s", SDL_GetRendererName(sdlRenderer));

        // On Retina/HiDPI displays the renderer output is in physical pixels while
        // SDL_GetWindowSize() (and therefore ImGui's io.DisplaySize) is in logical
        // points.  imgui_impl_sdlrenderer3 scales clip-rects via FramebufferScale
        // but does NOT call SDL_SetRenderScale(), so vertex positions stay at
        // logical-point coordinates inside the physical-pixel surface — producing
        // content that only fills 1/N of the screen.
        //
        // Fix: permanently set the renderer scale to the display's pixel ratio so
        // SDL maps every logical-point vertex and clip-rect to physical pixels.
        // imgui_impl_sdlrenderer3 detects a pre-existing scale != 1.0, skips its
        // own clip-rect upscaling (render_scale falls back to {1,1}), and defers
        // the logical→physical mapping entirely to SDL — which is correct.
        {
            float dscale = SDL_GetWindowDisplayScale(window);
            if (dscale > 1.0f)
                SDL_SetRenderScale(sdlRenderer, dscale, dscale);
        }

        currentWindow = new WindowHandle(window, WindowHandle::SDL3);
        currentWindow->sdlRenderer = sdlRenderer;
        return currentWindow;
#elif defined(__ANDROID__)
        // Android: Create fullscreen window
        SDL_Window* window = SDL_CreateWindow(
            title, width, height,
            SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_HIGH_PIXEL_DENSITY
        );

        if (!window) {
            __android_log_print(ANDROID_LOG_ERROR, "UAPMD", "SDL3 Window Error: %s", SDL_GetError());
            return nullptr;
        }

        __android_log_print(ANDROID_LOG_INFO, "UAPMD", "SDL3 window created for Android");
#else
        // Desktop: Create resizable window
        SDL_Window* window = SDL_CreateWindow(title, width, height,
            SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
        if (!window) {
            std::cerr << "SDL3 Window Error: " << SDL_GetError() << std::endl;
            return nullptr;
        }
#endif

#if !defined(USE_SDL_RENDERER)
        // Create GL context (works for both desktop and Android)
        glContext = SDL_GL_CreateContext(window);
        if (!glContext) {
#if defined(__ANDROID__)
            __android_log_print(ANDROID_LOG_ERROR, "UAPMD", "SDL3 GL Context Error: %s", SDL_GetError());
#else
            std::cerr << "SDL3 GL Context Error: " << SDL_GetError() << std::endl;
#endif
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
#endif // !USE_SDL_RENDERER
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
#if defined(USE_SDL_RENDERER)
            if (sdlRenderer)
                SDL_RenderPresent(sdlRenderer);
#else
            SDL_GL_SwapWindow(window->sdlWindow);
#endif
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
#if defined(USE_SDL_RENDERER)
            if (sdlRenderer) {
                SDL_DestroyRenderer(sdlRenderer);
                sdlRenderer = nullptr;
            }
#else
            if (glContext) {
                SDL_GL_DestroyContext(glContext);
                glContext = nullptr;
            }
#endif
            SDL_Quit();
            initialized = false;
        }
#if defined(__APPLE__) && !defined(USE_SDL_RENDERER)
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
#if defined(USE_SDL_RENDERER)
        return ImGui_ImplSDL3_InitForSDLRenderer(win->sdlWindow, win->sdlRenderer);
#else
        return ImGui_ImplSDL3_InitForOpenGL(win->sdlWindow, SDL_GL_GetCurrentContext());
#endif
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

std::unique_ptr<WindowingBackend> createSDL3WindowingBackend() {
    return std::make_unique<SDL3WindowingBackend>();
}

std::unique_ptr<ImGuiPlatformBackend> createSDL3ImGuiBackend() {
    return std::make_unique<SDL3ImGuiBackend>();
}

#if defined(USE_SDL_RENDERER)

class SDLRendererRenderer : public ImGuiRenderer {
    SDL_Renderer* renderer_ = nullptr;
public:
    bool initialize(WindowHandle* window) override {
        if (!window || !window->sdlRenderer) return false;
        renderer_ = window->sdlRenderer;
        return ImGui_ImplSDLRenderer3_Init(renderer_);
    }

    void newFrame() override {
        ImGui_ImplSDLRenderer3_NewFrame();
    }

    void renderDrawData() override {
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer_);
    }

    void shutdown() override {
        ImGui_ImplSDLRenderer3_Shutdown();
        renderer_ = nullptr;
    }

    const char* getName() const override {
        return "SDLRenderer3 (Metal)";
    }
};

std::unique_ptr<ImGuiRenderer> createSDLRendererRenderer() {
    return std::make_unique<SDLRendererRenderer>();
}

#endif // USE_SDL_RENDERER

} // namespace gui
} // namespace uapmd

#endif // USE_SDL3_BACKEND
