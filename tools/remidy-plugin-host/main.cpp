#include <remidy/remidy.hpp>
#include "AppModel.hpp"
#include "gui/MainWindow.hpp"
#include "ImGuiEventLoop.hpp"
#include <cpptrace/from_current.hpp>
#include <iostream>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>

static void glfw_error_callback(int error, const char* description) {
    std::cerr << "GLFW Error " << error << ": " << description << std::endl;
}

int runMain(int argc, char** argv) {
    // Initialize GLFW
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) {
        return EXIT_FAILURE;
    }

    // GL 3.2 + GLSL 150 for macOS compatibility
    const char* glsl_version = "#version 150";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

    // Create window - narrower main window
    GLFWwindow* window = glfwCreateWindow(640, 720, "Remidy Plugin Host", nullptr, nullptr);
    if (window == nullptr) {
        glfwTerminate();
        return EXIT_FAILURE;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // Enable vsync

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Setup Dear ImGui style
    ImGui::StyleColorsDark();

    // Setup Platform/Renderer backends
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    // Initialize Remidy event loop for ImGui
    auto eventLoop = std::make_unique<uapmd::gui::ImGuiEventLoop>();
    auto* eventLoopPtr = eventLoop.get();
    remidy::setEventLoop(eventLoop.release());
    remidy::EventLoop::initializeOnUIThread();

    // Initialize application model
    uapmd::AppModel::instantiate();

    // Create main window controller
    uapmd::gui::MainWindow mainWindow;

    // Start audio
    uapmd::AppModel::instance().sequencer().startAudio();

    // Main loop
    ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);
    while (!glfwWindowShouldClose(window) && mainWindow.isOpen()) {
        glfwPollEvents();

        // Process queued tasks from remidy
        eventLoopPtr->processQueuedTasks();

        // Start the Dear ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Render main window
        mainWindow.render(window);

        // Update (handles dialogs)
        mainWindow.update();

        // Rendering
        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(clear_color.x * clear_color.w, clear_color.y * clear_color.w,
                     clear_color.z * clear_color.w, clear_color.w);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    // Cleanup
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();

    uapmd::AppModel::cleanupInstance();

    return EXIT_SUCCESS;
}

int main(int argc, char** argv) {
    CPPTRACE_TRY {
        return runMain(argc, argv);
    } CPPTRACE_CATCH(const std::exception &e) {
        std::cerr << "Exception in remidy-plugin-host-imgui: " << e.what() << std::endl;
        cpptrace::from_current_exception().print();
        return EXIT_FAILURE;
    }
}