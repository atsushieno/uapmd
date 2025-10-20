#include "PlatformEmbedHost.hpp"

#if defined(USE_GLFW_BACKEND)
 #include <GLFW/glfw3.h>
 #define GLFW_EXPOSE_NATIVE_X11
 #include <GLFW/glfw3native.h>
#endif
#if defined(USE_SDL2_BACKEND)
 #include <SDL.h>
 #include <SDL_syswm.h>
#endif
#if defined(USE_SDL3_BACKEND)
 #include <SDL3/SDL.h>
 #include <SDL3/SDL_syswm.h>
#endif
#if defined(__linux__) && !defined(__APPLE__)
 #include <X11/Xlib.h>
#endif

namespace uapmd::gui {

PlatformEmbedHost::PlatformEmbedHost(void* hostWindow) : hostWindow_(hostWindow) {
#if defined(__linux__) && !defined(__APPLE__)
  #if defined(USE_GLFW_BACKEND)
    // Initialise X11 state from the GLFW window
    auto* glfwWin = static_cast<GLFWwindow*>(hostWindow_);
    if (glfwWin) {
        auto* dpy = glfwGetX11Display();
        auto parent = glfwGetX11Window(glfwWin);
        x11_.display = dpy;
        x11_.parent = static_cast<std::uintptr_t>(parent);
        x11_.child = 0;
    }
  #elif defined(USE_SDL2_BACKEND)
    // Initialise X11 state from SDL2 window
    auto* sdlWin = static_cast<SDL_Window*>(hostWindow_);
    if (sdlWin) {
        SDL_SysWMinfo info;
        SDL_VERSION(&info.version);
        if (SDL_GetWindowWMInfo(sdlWin, &info)) {
            #if defined(SDL_VIDEO_DRIVER_X11)
            x11_.display = reinterpret_cast<void*>(info.info.x11.display);
            x11_.parent = static_cast<std::uintptr_t>(info.info.x11.window);
            x11_.child = 0;
            #endif
        }
    }
  #elif defined(USE_SDL3_BACKEND)
    // Initialise X11 state from SDL3 window
    auto* sdlWin = static_cast<SDL_Window*>(hostWindow_);
    if (sdlWin) {
        SDL_SysWMinfo info{};
        if (SDL_GetWindowWMInfo(sdlWin, &info)) {
            #if defined(SDL_VIDEO_DRIVER_X11)
            x11_.display = reinterpret_cast<void*>(info.info.x11.display);
            x11_.parent = static_cast<std::uintptr_t>(info.info.x11.window);
            x11_.child = 0;
            #endif
        }
    }
  #else
    (void) hostWindow_;
  #endif
#else
    (void) hostWindow_;
#endif
}

PlatformEmbedHost::~PlatformEmbedHost() {
    destroy();
}

bool PlatformEmbedHost::createChild(int x, int y, int width, int height) {
#if defined(__linux__) && !defined(__APPLE__)
    if (!x11_.display || x11_.parent == 0)
        return false;
    if (x11_.child != 0)
        return true; // already created

    Display* dpy = static_cast<Display*>(x11_.display);
    Window parent = static_cast<Window>(x11_.parent);

    XSetWindowAttributes swa{};
    swa.event_mask = ExposureMask | StructureNotifyMask | FocusChangeMask;
    swa.background_pixmap = None; // avoid black clears
    swa.backing_store = NotUseful;

    Window child = XCreateWindow(dpy,
                                 parent,
                                 x, y,
                                 (unsigned) width, (unsigned) height,
                                 0, // border width
                                 CopyFromParent, // depth
                                 InputOutput,
                                 CopyFromParent, // visual
                                 CWEventMask | CWBackPixmap | CWBackingStore,
                                 &swa);
    if (!child)
        return false;
    // Raise and map to ensure it’s visible above GL content
    XMapRaised(dpy, child);
    XFlush(dpy);
    x11_.child = static_cast<std::uintptr_t>(child);
    return true;
#else
    (void) x; (void) y; (void) width; (void) height;
    return false;
#endif
}

void PlatformEmbedHost::setBounds(int x, int y, int width, int height) {
#if defined(__linux__) && !defined(__APPLE__)
    if (!x11_.display || x11_.child == 0)
        return;
    Display* dpy = static_cast<Display*>(x11_.display);
    Window child = static_cast<Window>(x11_.child);
    XMoveResizeWindow(dpy, child, x, y, (unsigned) width, (unsigned) height);
    XFlush(dpy);
#else
    (void) x; (void) y; (void) width; (void) height;
#endif
}

void* PlatformEmbedHost::nativeParentHandle() const {
#if defined(__linux__) && !defined(__APPLE__)
    return reinterpret_cast<void*>(x11_.child);
#else
    return nullptr;
#endif
}

void PlatformEmbedHost::destroy() {
#if defined(__linux__) && !defined(__APPLE__)
    if (!x11_.display || x11_.child == 0)
        return;
    Display* dpy = static_cast<Display*>(x11_.display);
    Window child = static_cast<Window>(x11_.child);
    XUnmapWindow(dpy, child);
    XDestroyWindow(dpy, child);
    XFlush(dpy);
    x11_.child = 0;
#endif
}

} // namespace uapmd::gui
