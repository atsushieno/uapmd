#pragma once

// GL Context Guard Utility
// Saves and restores the current OpenGL context to protect against plugins that
// might change the active GL context without restoring it (e.g., Kontakt 8)

#if defined(__APPLE__)
    #include <OpenGL/OpenGL.h>
#elif defined(_WIN32)
    #include <windows.h>
    #include <wingdi.h>
#elif defined(__linux__)
    #include <GL/glx.h>
#endif

namespace remidy::gui {

    #if defined(__APPLE__)
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
    #elif defined(__linux__)
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
        class GLContextGuard {
        public:
            GLContextGuard() {}
            ~GLContextGuard() {}
        };
    #endif

} // namespace remidy::gui
