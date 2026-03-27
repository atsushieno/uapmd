#pragma once

// GL Context Guard Utility
// Saves and restores the current OpenGL context to protect against plugins that
// might change the active GL context without restoring it (e.g., Kontakt 8)

// TargetConditionals.h provides TARGET_OS_IPHONE, which is used below to
// exclude the CGL (CoreGL) path on iOS where CGL does not exist.
#if defined(__APPLE__)
    #include <TargetConditionals.h>
#endif

// When using DirectX renderer, OpenGL context management is not needed
#ifndef USE_DIRECTX11_RENDERER
    #if defined(__ANDROID__)
        #include <GLES3/gl3.h>
        #include <GLES3/gl3ext.h>
        #include <SDL3/SDL_opengl.h>
    #elif defined(__APPLE__) && !TARGET_OS_IPHONE
        // macOS only: CGL is not available on iOS (which uses Metal/SDL_Renderer).
        #include <OpenGL/OpenGL.h>
    #elif defined(_WIN32)
        #include <windows.h>
        #include <wingdi.h>
    #elif defined(__linux__) && !defined(__ANDROID__)
        #include <GL/glx.h>
    #endif
#endif

namespace remidy::gui {

    #if defined(USE_DIRECTX11_RENDERER)
        // No-op implementation for DirectX 11 renderer (no OpenGL context to guard)
        class GLContextGuard {
        public:
            GLContextGuard() {}
            ~GLContextGuard() {}
            GLContextGuard(const GLContextGuard&) = delete;
            GLContextGuard& operator=(const GLContextGuard&) = delete;
        };
    #elif defined(__APPLE__) && !TARGET_OS_IPHONE
        // macOS only: save/restore the CGL context so plugins cannot steal it.
        // On iOS the Metal/SDL_Renderer path has no OpenGL context to guard.
        class GLContextGuard {
        private:
            CGLContextObj saved_context;

        public:
            GLContextGuard() : saved_context(CGLGetCurrentContext()) {}

            ~GLContextGuard() {
                if (saved_context != nullptr)
                    CGLSetCurrentContext(saved_context);
            }

            GLContextGuard(const GLContextGuard&) = delete;
            GLContextGuard& operator=(const GLContextGuard&) = delete;
        };
    #elif defined(_WIN32)
        class GLContextGuard {
        private:
            HGLRC saved_context;
            HDC saved_dc;

        public:
            GLContextGuard()
                : saved_context(wglGetCurrentContext()), saved_dc(wglGetCurrentDC()) {}

            ~GLContextGuard() {
                if (saved_context && saved_dc)
                    wglMakeCurrent(saved_dc, saved_context);
            }

            GLContextGuard(const GLContextGuard&) = delete;
            GLContextGuard& operator=(const GLContextGuard&) = delete;
        };
    #elif defined(__linux__) && !defined(__ANDROID__)
        class GLContextGuard {
        private:
            Display* saved_display;
            GLXDrawable saved_drawable;
            GLXContext saved_context;

        public:
            GLContextGuard()
                : saved_display(glXGetCurrentDisplay()),
                  saved_drawable(glXGetCurrentDrawable()),
                  saved_context(glXGetCurrentContext()) {}

            ~GLContextGuard() {
                if (saved_display != nullptr && saved_drawable != 0 && saved_context != nullptr)
                    glXMakeCurrent(saved_display, saved_drawable, saved_context);
            }

            GLContextGuard(const GLContextGuard&) = delete;
            GLContextGuard& operator=(const GLContextGuard&) = delete;
        };
    #else
        // No-op for platforms without an OpenGL context to save/restore
        // (iOS with Metal/SDL_Renderer, Android, Emscripten, etc.)
        class GLContextGuard {
        public:
            GLContextGuard() {}
            ~GLContextGuard() {}
        };
    #endif

} // namespace remidy::gui
