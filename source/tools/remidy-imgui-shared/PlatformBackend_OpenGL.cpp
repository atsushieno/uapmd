#include "PlatformBackend.hpp"

#if !defined(USE_DIRECTX11_RENDERER) && !defined(USE_SDL_RENDERER)

#include <imgui.h>
#include <imgui_impl_opengl3.h>

namespace uapmd {
namespace gui {

class OpenGL3Renderer : public ImGuiRenderer {
public:
    bool initialize(WindowHandle* /*window*/) override {
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

std::unique_ptr<ImGuiRenderer> createOpenGL3Renderer() {
    return std::make_unique<OpenGL3Renderer>();
}

} // namespace gui
} // namespace uapmd

#endif // !defined(USE_DIRECTX11_RENDERER) && !defined(USE_SDL_RENDERER)
