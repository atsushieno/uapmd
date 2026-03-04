#include "PlatformBackend.hpp"

#include <stdexcept>

namespace uapmd {
namespace gui {

#ifdef USE_WIN32_BACKEND
std::unique_ptr<WindowingBackend> createWin32WindowingBackend();
std::unique_ptr<ImGuiPlatformBackend> createWin32ImGuiBackend();
#endif

#ifdef USE_SDL3_BACKEND
std::unique_ptr<WindowingBackend> createSDL3WindowingBackend();
std::unique_ptr<ImGuiPlatformBackend> createSDL3ImGuiBackend();
#endif

#ifdef USE_SDL2_BACKEND
std::unique_ptr<WindowingBackend> createSDL2WindowingBackend();
std::unique_ptr<ImGuiPlatformBackend> createSDL2ImGuiBackend();
#endif

#ifdef USE_GLFW_BACKEND
std::unique_ptr<WindowingBackend> createGLFWWindowingBackend();
std::unique_ptr<ImGuiPlatformBackend> createGLFWImGuiBackend();
#endif

#ifdef USE_DIRECTX11_RENDERER
std::unique_ptr<ImGuiRenderer> createDirectX11Renderer();
#endif

#if defined(USE_SDL_RENDERER) && defined(USE_SDL3_BACKEND)
std::unique_ptr<ImGuiRenderer> createSDLRendererRenderer();
#endif

#if !defined(USE_DIRECTX11_RENDERER) && !defined(USE_SDL_RENDERER)
std::unique_ptr<ImGuiRenderer> createOpenGL3Renderer();
#endif

std::unique_ptr<WindowingBackend> WindowingBackend::create() {
#ifdef USE_WIN32_BACKEND
    return createWin32WindowingBackend();
#elif defined(USE_SDL3_BACKEND)
    return createSDL3WindowingBackend();
#elif defined(USE_SDL2_BACKEND)
    return createSDL2WindowingBackend();
#elif defined(USE_GLFW_BACKEND)
    return createGLFWWindowingBackend();
#else
    throw std::runtime_error("No windowing backend available");
#endif
}

std::unique_ptr<ImGuiPlatformBackend> ImGuiPlatformBackend::create(WindowHandle* window) {
    if (!window) {
        return nullptr;
    }

    switch (window->type) {
#ifdef USE_WIN32_BACKEND
        case WindowHandle::Win32:
            return createWin32ImGuiBackend();
#endif
#ifdef USE_SDL3_BACKEND
        case WindowHandle::SDL3:
            return createSDL3ImGuiBackend();
#endif
#ifdef USE_SDL2_BACKEND
        case WindowHandle::SDL2:
            return createSDL2ImGuiBackend();
#endif
#ifdef USE_GLFW_BACKEND
        case WindowHandle::GLFW:
            return createGLFWImGuiBackend();
#endif
        default:
            return nullptr;
    }
}

std::unique_ptr<ImGuiRenderer> ImGuiRenderer::create() {
#ifdef USE_DIRECTX11_RENDERER
    return createDirectX11Renderer();
#elif defined(USE_SDL_RENDERER) && defined(USE_SDL3_BACKEND)
    return createSDLRendererRenderer();
#else
    return createOpenGL3Renderer();
#endif
}

} // namespace gui
} // namespace uapmd
