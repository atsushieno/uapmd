#pragma once

#include <cstdint>

namespace uapmd::gui {

// Lightweight native child-container for embedding plugin UIs.
// Currently implemented for GLFW + X11 on Linux. Other platforms can be added.
class PlatformEmbedHost {
public:
    // Construct with the platform window pointer passed from MainWindow::render
    // For GLFW backend, this is a GLFWwindow*.
    explicit PlatformEmbedHost(void* hostWindow);
    ~PlatformEmbedHost();

    // Create a native child container at given position/size (in window coordinates, pixels)
    bool createChild(int x, int y, int width, int height);

    // Move/resize the native child
    void setBounds(int x, int y, int width, int height);

    // Handle to pass to CLAP set_parent (XID on X11)
    void* nativeParentHandle() const;

    // Destroy child container if created
    void destroy();

private:
    void* hostWindow_ = nullptr;

#if defined(__linux__) && !defined(__APPLE__)
    // X11 state (only when using GLFW backend)
    struct X11State {
        void* display = nullptr; // Display*
        std::uintptr_t parent = 0; // Window
        std::uintptr_t child = 0;  // Window
    };
    X11State x11_{};
#endif
};

} // namespace uapmd::gui

