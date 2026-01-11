// Web main integrating the actual GUI with web stubs
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>
#include <emscripten.h>
#include <emscripten/html5.h>
#include <iostream>

#include "stubs/AppModel.hpp"         // Web AppModel stub
#include "../tools/uapmd-app/gui/MainWindow.hpp"

struct WasmContext {
    GLFWwindow* window = nullptr;
    ImVec4 clearColor{0.45f, 0.55f, 0.60f, 1.00f};
    std::unique_ptr<uapmd::gui::MainWindow> mainWindow;
};

static WasmContext g_ctx;

static void glfw_error_callback(int error, const char* description) {
    std::cerr << "GLFW Error " << error << ": " << description << std::endl;
}

void mainLoopIteration(void*) {
    glfwPollEvents();
    if (glfwWindowShouldClose(g_ctx.window)) {
        emscripten_cancel_main_loop();
        return;
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    // Render desktop GUI in the web frame
    if (g_ctx.mainWindow) {
        g_ctx.mainWindow->render(static_cast<void*>(g_ctx.window));
        g_ctx.mainWindow->update();
    }

    ImGui::Render();

    int display_w, display_h;
    glfwGetFramebufferSize(g_ctx.window, &display_w, &display_h);
    glViewport(0, 0, display_w, display_h);
    glClearColor(g_ctx.clearColor.x * g_ctx.clearColor.w,
                 g_ctx.clearColor.y * g_ctx.clearColor.w,
                 g_ctx.clearColor.z * g_ctx.clearColor.w,
                 g_ctx.clearColor.w);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

int main(int, char**) {
    std::cout << "Initializing uapmd-app (Web) with GUI..." << std::endl;

    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) {
        std::cerr << "Failed to initialize GLFW" << std::endl;
        return EXIT_FAILURE;
    }

    // Request WebGL 2 (maps to OpenGL ES 3.0). Emscripten's GLFW expects
    // WebGL major versions 1 or 2; requesting 3 is invalid and fails.
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);

    g_ctx.window = glfwCreateWindow(1024, 768, "uapmd-app (Web)", nullptr, nullptr);
    if (!g_ctx.window) {
        std::cerr << "Failed to create GLFW window" << std::endl;
        glfwTerminate();
        return EXIT_FAILURE;
    }

    glfwMakeContextCurrent(g_ctx.window);
    // On Emscripten, requestAnimationFrame drives vsync; avoid glfwSwapInterval which
    // may call emscripten_set_main_loop_timing before a main loop is registered.
#if !defined(EMSCRIPTEN)
    glfwSwapInterval(1);
#endif

    // Log GL info to aid debugging version mismatches on web
    const GLubyte* glVersion = glGetString(GL_VERSION);
    const GLubyte* glRenderer = glGetString(GL_RENDERER);
    if (glVersion && glRenderer) {
        std::cout << "GL Version: " << glVersion << " | Renderer: " << glRenderer << std::endl;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();

    ImGui_ImplGlfw_InitForOpenGL(g_ctx.window, true);
    ImGui_ImplOpenGL3_Init("#version 300 es");

    // Initialize application model and GUI
    uapmd::AppModel::instantiate();
    g_ctx.mainWindow = std::make_unique<uapmd::gui::MainWindow>(uapmd::gui::GuiDefaults{});

    emscripten_set_main_loop_arg(mainLoopIteration, nullptr, 0, true);

    // Cleanup (unreached on browser, kept for completeness)
    g_ctx.mainWindow.reset();
    uapmd::AppModel::cleanupInstance();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(g_ctx.window);
    glfwTerminate();

    return EXIT_SUCCESS;
}
