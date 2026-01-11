#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>
#include <emscripten.h>
#include <emscripten/html5.h>
#include <iostream>

struct WasmContext {
    GLFWwindow* window = nullptr;
    ImVec4 clearColor{0.45f, 0.55f, 0.60f, 1.00f};
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

    ImGui::Begin("uapmd-app - WebAssembly");
    ImGui::Text("uapmd-app for WebAssembly");
    ImGui::Text("This is a work in progress port.");
    ImGui::Separator();
    ImGui::Text("Status: ImGui rendering successfully with GLFW!");
    ImGui::Text("Next steps:");
    ImGui::BulletText("Port uapmd AppModel and GUI components");
    ImGui::BulletText("Add stub implementations for audio/plugin APIs");
    ImGui::BulletText("Integrate with Web MIDI and Web Audio APIs");
    ImGui::End();

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

int main(int argc, char** argv) {
    std::cout << "Initializing uapmd-app for WebAssembly..." << std::endl;

    glfwSetErrorCallback(glfw_error_callback);

    if (!glfwInit()) {
        std::cerr << "Failed to initialize GLFW" << std::endl;
        return EXIT_FAILURE;
    }

    // Request WebGL2 (maps to OpenGL ES 3.0). Emscripten's GLFW expects
    // major versions 1 or 2; 2 selects WebGL2.
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);

    g_ctx.window = glfwCreateWindow(1024, 768, "uapmd-app (WebAssembly)", nullptr, nullptr);
    if (!g_ctx.window) {
        std::cerr << "Failed to create GLFW window" << std::endl;
        glfwTerminate();
        return EXIT_FAILURE;
    }

    glfwMakeContextCurrent(g_ctx.window);
    // Emscripten uses requestAnimationFrame; do not set swap interval before main loop.
#if !defined(EMSCRIPTEN)
    glfwSwapInterval(1); // Enable vsync on native
#endif

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;

    ImGui::StyleColorsDark();

    ImGui_ImplGlfw_InitForOpenGL(g_ctx.window, true);
    ImGui_ImplOpenGL3_Init("#version 300 es");

#ifdef EMSCRIPTEN
    double canvas_css_w = 0.0;
    double canvas_css_h = 0.0;
    emscripten_get_element_css_size("#canvas", &canvas_css_w, &canvas_css_h);
    const double dpr = emscripten_get_device_pixel_ratio();
    emscripten_set_canvas_element_size("#canvas", static_cast<int>(canvas_css_w * dpr), static_cast<int>(canvas_css_h * dpr));
    emscripten_set_resize_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, false,
        [](int, const EmscriptenUiEvent*, void*) -> EM_BOOL {
            double canvas_css_width2 = 0.0;
            double canvas_css_height2 = 0.0;
            double d = emscripten_get_device_pixel_ratio();
            emscripten_get_element_css_size("#canvas", &canvas_css_width2, &canvas_css_height2);
            emscripten_set_canvas_element_size("#canvas", static_cast<int>(canvas_css_width2 * d), static_cast<int>(canvas_css_height2 * d));
            return EM_TRUE;
        });
#endif

    emscripten_set_main_loop_arg(mainLoopIteration, nullptr, 0, true);

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(g_ctx.window);
    glfwTerminate();

    return EXIT_SUCCESS;
}
