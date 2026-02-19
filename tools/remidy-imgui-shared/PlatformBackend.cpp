#include "PlatformBackend.hpp"
#include <iostream>
#include <stdexcept>

#ifdef ANDROID
#include <android/log.h>
#endif

#ifdef __APPLE__
#include <choc/platform/choc_ObjectiveCHelpers.h>
#endif

// Include headers conditionally based on compile-time definitions
#ifdef USE_WIN32_BACKEND
    #include <windows.h>
    #include <windowsx.h>
    #include <imgui_impl_win32.h>
    #ifdef USE_DIRECTX11_RENDERER
        #include <d3d11.h>
        #include <dxgi.h>
        #include <imgui_impl_dx11.h>
    #endif
    // Forward declare ImGui Win32 WndProc handler
    extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
#endif

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

#ifndef USE_DIRECTX11_RENDERER
#include <imgui_impl_opengl3.h>
#endif
#include <imgui.h>

namespace uapmd {
namespace gui {

// =============================================================================
// Win32 Backend Implementation
// =============================================================================
#ifdef USE_WIN32_BACKEND

// Global quit flag for Win32
static bool g_Win32_QuitRequested = false;

class Win32WindowingBackend : public WindowingBackend {
private:
    bool initialized = false;
    WindowHandle* currentWindow = nullptr;
    HINSTANCE hInstance = nullptr;
    const wchar_t* windowClassName = L"UAPMDWindowClass";

    static LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
        // Forward messages to ImGui Win32 handler
        if (ImGui_ImplWin32_WndProcHandler(hWnd, message, wParam, lParam))
            return true;

        switch (message) {
        case WM_SIZE:
            // Window resize handled by DirectX renderer
            return 0;
        case WM_CLOSE:
            g_Win32_QuitRequested = true;
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hWnd, message, wParam, lParam);
        }
    }

public:
    bool initialize() override {
        hInstance = GetModuleHandle(nullptr);

        // Set DPI awareness
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

        // Register window class
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
        wc.lpfnWndProc = WndProc;
        wc.cbClsExtra = 0;
        wc.cbWndExtra = 0;
        wc.hInstance = hInstance;
        wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszMenuName = nullptr;
        wc.lpszClassName = windowClassName;
        wc.hIconSm = LoadIcon(nullptr, IDI_APPLICATION);

        if (!RegisterClassExW(&wc)) {
            std::cerr << "Win32 Error: Failed to register window class" << std::endl;
            return false;
        }

        initialized = true;
        return true;
    }

    WindowHandle* createWindow(const char* title, int width, int height) override {
        if (!initialized) return nullptr;

        // Convert title to wide string
        int titleLen = MultiByteToWideChar(CP_UTF8, 0, title, -1, nullptr, 0);
        wchar_t* wideTitle = new wchar_t[titleLen];
        MultiByteToWideChar(CP_UTF8, 0, title, -1, wideTitle, titleLen);

        // Create window
        DWORD dwStyle = WS_OVERLAPPEDWINDOW;
        RECT rect = { 0, 0, width, height };
        AdjustWindowRect(&rect, dwStyle, FALSE);

        HWND hwnd = CreateWindowExW(
            0,
            windowClassName,
            wideTitle,
            dwStyle,
            CW_USEDEFAULT, CW_USEDEFAULT,
            rect.right - rect.left, rect.bottom - rect.top,
            nullptr,
            nullptr,
            hInstance,
            nullptr
        );

        delete[] wideTitle;

        if (!hwnd) {
            std::cerr << "Win32 Error: Failed to create window" << std::endl;
            return nullptr;
        }

        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);

        currentWindow = new WindowHandle(hwnd);
        return currentWindow;
    }

    void destroyWindow(WindowHandle* window) override {
        if (window && window->type == WindowHandle::Win32) {
            DestroyWindow(window->hwnd);
            delete window;
            if (window == currentWindow) currentWindow = nullptr;
        }
    }

    bool shouldClose(WindowHandle* window) override {
        return g_Win32_QuitRequested || window == nullptr;
    }

    void swapBuffers(WindowHandle* window) override {
        // No-op for DirectX (handled by renderer Present())
    }

    void getDrawableSize(WindowHandle* window, int* width, int* height) override {
        if (window && window->type == WindowHandle::Win32) {
            RECT rect;
            GetClientRect(window->hwnd, &rect);
            *width = rect.right - rect.left;
            *height = rect.bottom - rect.top;
        }
    }

    void setWindowSize(WindowHandle* window, int width, int height) override {
        if (window && window->type == WindowHandle::Win32) {
            RECT rect = { 0, 0, width, height };
            AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
            SetWindowPos(window->hwnd, nullptr, 0, 0,
                        rect.right - rect.left, rect.bottom - rect.top,
                        SWP_NOMOVE | SWP_NOZORDER);
        }
    }

    void makeContextCurrent(WindowHandle* window) override {
        // No-op for DirectX (no context to make current)
    }

    void shutdown() override {
        if (initialized) {
            UnregisterClassW(windowClassName, hInstance);
            initialized = false;
        }
    }

    const char* getName() const override {
        return "Win32";
    }
};

class Win32ImGuiBackend : public ImGuiPlatformBackend {
private:
    WindowHandle* window = nullptr;

public:
    bool initialize(WindowHandle* win) override {
        if (!win || win->type != WindowHandle::Win32) return false;
        window = win;
        return ImGui_ImplWin32_Init(win->hwnd);
    }

    void processEvents() override {
        // Process Win32 messages
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) {
                g_Win32_QuitRequested = true;
            }
        }
    }

    void newFrame() override {
        ImGui_ImplWin32_NewFrame();
    }

    void shutdown() override {
        ImGui_ImplWin32_Shutdown();
    }

    const char* getName() const override {
        return "Win32 ImGui Backend";
    }
};

#endif // USE_WIN32_BACKEND

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
#if defined(__ANDROID__)
        // Android: Initialize SDL3 with OpenGL ES 3.0
        #include <android/log.h>
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
            __android_log_print(ANDROID_LOG_ERROR, "UAPMD", "SDL3 Init Error: %s", SDL_GetError());
            return false;
        }

        // Configure for OpenGL ES 3.0 on Android
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);

        __android_log_print(ANDROID_LOG_INFO, "UAPMD", "SDL3 initialized for Android/GLES3");
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

#if defined(__ANDROID__)
        // Android: Create fullscreen window
        #include <android/log.h>
        SDL_Window* window = SDL_CreateWindow(
            title, width, height,
            SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN
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
// DirectX 11 Renderer Implementation
// =============================================================================
#ifdef USE_DIRECTX11_RENDERER

class DirectX11Renderer : public ImGuiRenderer {
private:
    ID3D11Device*           g_pd3dDevice = nullptr;
    ID3D11DeviceContext*    g_pd3dDeviceContext = nullptr;
    IDXGISwapChain*         g_pSwapChain = nullptr;
    ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;

    bool                    g_SwapChainOccluded = false;
    UINT                    g_ResizeWidth = 0;
    UINT                    g_ResizeHeight = 0;

    void CreateRenderTarget() {
        ID3D11Texture2D* pBackBuffer = nullptr;
        g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
        if (pBackBuffer) {
            g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
            pBackBuffer->Release();
        }
    }

    void CleanupRenderTarget() {
        if (g_mainRenderTargetView) {
            g_mainRenderTargetView->Release();
            g_mainRenderTargetView = nullptr;
        }
    }

    bool CreateDeviceD3D(HWND hWnd) {
        // Setup swap chain
        DXGI_SWAP_CHAIN_DESC sd;
        ZeroMemory(&sd, sizeof(sd));
        sd.BufferCount = 2;
        sd.BufferDesc.Width = 0;
        sd.BufferDesc.Height = 0;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferDesc.RefreshRate.Numerator = 60;
        sd.BufferDesc.RefreshRate.Denominator = 1;
        sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = hWnd;
        sd.SampleDesc.Count = 1;
        sd.SampleDesc.Quality = 0;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        UINT createDeviceFlags = 0;
        #ifdef _DEBUG
        createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
        #endif

        D3D_FEATURE_LEVEL featureLevel;
        const D3D_FEATURE_LEVEL featureLevelArray[2] = {
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_0,
        };

        HRESULT res = D3D11CreateDeviceAndSwapChain(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            createDeviceFlags,
            featureLevelArray,
            2,
            D3D11_SDK_VERSION,
            &sd,
            &g_pSwapChain,
            &g_pd3dDevice,
            &featureLevel,
            &g_pd3dDeviceContext);

        // Try WARP software renderer if hardware failed
        if (res == DXGI_ERROR_UNSUPPORTED) {
            res = D3D11CreateDeviceAndSwapChain(
                nullptr,
                D3D_DRIVER_TYPE_WARP,
                nullptr,
                createDeviceFlags,
                featureLevelArray,
                2,
                D3D11_SDK_VERSION,
                &sd,
                &g_pSwapChain,
                &g_pd3dDevice,
                &featureLevel,
                &g_pd3dDeviceContext);
        }

        if (res != S_OK)
            return false;

        CreateRenderTarget();
        return true;
    }

    void CleanupDeviceD3D() {
        CleanupRenderTarget();
        if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
        if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
        if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
    }

public:
    bool initialize(WindowHandle* window) override {
        if (!window || window->type != WindowHandle::Win32) {
            std::cerr << "DirectX11Renderer Error: Window is not Win32 type" << std::endl;
            return false;
        }

        HWND hWnd = window->hwnd;
        if (!hWnd) {
            std::cerr << "DirectX11Renderer Error: Invalid HWND" << std::endl;
            return false;
        }

        if (!CreateDeviceD3D(hWnd)) {
            std::cerr << "DirectX11Renderer Error: Failed to create D3D11 device" << std::endl;
            return false;
        }

        if (!ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext)) {
            std::cerr << "DirectX11Renderer Error: Failed to initialize ImGui DX11 backend" << std::endl;
            CleanupDeviceD3D();
            return false;
        }

        return true;
    }

    void newFrame() override {
        ImGui_ImplDX11_NewFrame();
    }

    void renderDrawData() override {
        // Check if swap chain needs resizing based on ImGui's display size
        ImGuiIO& io = ImGui::GetIO();
        UINT targetWidth = (UINT)io.DisplaySize.x;
        UINT targetHeight = (UINT)io.DisplaySize.y;

        if (targetWidth > 0 && targetHeight > 0) {
            // Get current swap chain dimensions
            ID3D11Texture2D* pBackBuffer = nullptr;
            g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
            D3D11_TEXTURE2D_DESC desc;
            pBackBuffer->GetDesc(&desc);
            pBackBuffer->Release();

            // Resize swap chain if dimensions don't match
            if (desc.Width != targetWidth || desc.Height != targetHeight) {
                CleanupRenderTarget();
                // Unbind all render targets and flush before ResizeBuffers;
                // any lingering OM references to swap chain buffers will cause
                // ResizeBuffers to return DXGI_ERROR_INVALID_CALL and leave the
                // swap chain broken, crashing Present on the next frame.
                g_pd3dDeviceContext->OMSetRenderTargets(0, nullptr, nullptr);
                g_pd3dDeviceContext->Flush();
                HRESULT resizeHr = g_pSwapChain->ResizeBuffers(0, targetWidth, targetHeight, DXGI_FORMAT_UNKNOWN, 0);
                if (SUCCEEDED(resizeHr))
                    CreateRenderTarget();
            }
        }

        // Get current swap chain dimensions for viewport
        ID3D11Texture2D* pBackBuffer = nullptr;
        g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
        D3D11_TEXTURE2D_DESC desc;
        pBackBuffer->GetDesc(&desc);
        pBackBuffer->Release();

        // Set viewport
        D3D11_VIEWPORT vp;
        vp.Width = (FLOAT)desc.Width;
        vp.Height = (FLOAT)desc.Height;
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        vp.TopLeftX = 0;
        vp.TopLeftY = 0;
        g_pd3dDeviceContext->RSSetViewports(1, &vp);

        // Clear and render
        const float clear_color[4] = { 0.45f, 0.55f, 0.60f, 1.00f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color);

        // Render ImGui draw data
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        // Present with VSync
        HRESULT hr = g_pSwapChain->Present(1, 0);
        g_SwapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);
    }

    void shutdown() override {
        ImGui_ImplDX11_Shutdown();
        CleanupDeviceD3D();
    }

    const char* getName() const override {
        return "DirectX11";
    }

    void handleResize(int width, int height) {
        g_ResizeWidth = (UINT)width;
        g_ResizeHeight = (UINT)height;
    }
};

#endif // USE_DIRECTX11_RENDERER

// =============================================================================
// OpenGL3 Renderer Implementation
// =============================================================================
#ifndef USE_DIRECTX11_RENDERER

class OpenGL3Renderer : public ImGuiRenderer {
public:
    bool initialize(WindowHandle* window) override {
#if defined(__ANDROID__)
        // Android: Use OpenGL ES 3.0 with GLSL ES 3.00
        return ImGui_ImplOpenGL3_Init("#version 300 es");
#elif defined(_WIN32)
        // Windows: Use nullptr to let ImGui auto-detect GLSL version
        // This enables ImGui's built-in GL loader and works with older drivers
        // Passing nullptr uses GLSL 1.30 which is more compatible than 1.50
        return ImGui_ImplOpenGL3_Init(nullptr);
#else
        // Desktop (macOS/Linux): Use OpenGL 3.2 with GLSL 1.50
        return ImGui_ImplOpenGL3_Init("#version 150");
#endif
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

#endif // !USE_DIRECTX11_RENDERER

// =============================================================================
// Factory Methods
// =============================================================================

std::unique_ptr<WindowingBackend> WindowingBackend::create() {
#ifdef USE_WIN32_BACKEND
    return std::make_unique<Win32WindowingBackend>();
#elif defined(USE_SDL3_BACKEND)
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
#ifdef USE_WIN32_BACKEND
        case WindowHandle::Win32:
            return std::make_unique<Win32ImGuiBackend>();
#endif
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
#ifdef USE_DIRECTX11_RENDERER
    return std::make_unique<DirectX11Renderer>();
#else
    return std::make_unique<OpenGL3Renderer>();
#endif
}

} // namespace gui
} // namespace uapmd
