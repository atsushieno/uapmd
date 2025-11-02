#include <remidy/remidy.hpp>
#include "AppModel.hpp"
#include "gui/MainWindow.hpp"
#include "ImGuiEventLoop.hpp"
#include "gui/PlatformBackend.hpp"
#include <cpptrace/from_current.hpp>
#include <iostream>

#include <imgui.h>

#ifdef USE_SDL2_BACKEND
    #include <SDL_opengl.h>
#elif defined(USE_SDL3_BACKEND)
    #if defined(__APPLE__)
        #include <OpenGL/gl3.h>
    #else
        #include <SDL3/SDL_opengl.h>
    #endif
#elif defined(USE_GLFW_BACKEND)
    #include <GLFW/glfw3.h>
    #include <GL/gl.h>
#endif

using namespace uapmd::gui;

int runMain(int argc, char** argv) {
    // Create windowing backend with priority: SDL3 > SDL2 > GLFW
    auto windowingBackend = WindowingBackend::create();
    if (!windowingBackend) {
        std::cerr << "Error: No suitable windowing backend found" << std::endl;
        return EXIT_FAILURE;
    }

    // Initialize windowing system
    if (!windowingBackend->initialize()) {
        std::cerr << "Error: Failed to initialize " << windowingBackend->getName() << " backend" << std::endl;
        return EXIT_FAILURE;
    }

    // backend initialized

    // Create window
    WindowHandle* window = windowingBackend->createWindow("Remidy Plugin Host", 640, 800);
    if (!window) {
        std::cerr << "Error: Failed to create window" << std::endl;
        windowingBackend->shutdown();
        return EXIT_FAILURE;
    }

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Setup Dear ImGui style
    ImGui::StyleColorsDark();

    // Apply DPI scaling (simplified for now)
    float dpi_scale = 1.0f;
    // TODO: Add platform-specific DPI detection through backend abstraction

    if (dpi_scale > 1.1f && dpi_scale <= 3.0f) {
        ImGuiStyle& style = ImGui::GetStyle();
        style.ScaleAllSizes(dpi_scale);
    }

    // Create ImGui platform and renderer backends
    auto imguiPlatformBackend = ImGuiPlatformBackend::create(window);
    auto imguiRenderer = ImGuiRenderer::create();

    if (!imguiPlatformBackend || !imguiRenderer) {
        std::cerr << "Error: Failed to create ImGui backends" << std::endl;
        ImGui::DestroyContext();
        windowingBackend->destroyWindow(window);
        windowingBackend->shutdown();
        return EXIT_FAILURE;
    }

    // Initialize ImGui backends
    if (!imguiPlatformBackend->initialize(window) || !imguiRenderer->initialize(window)) {
        std::cerr << "Error: Failed to initialize ImGui backends" << std::endl;
        ImGui::DestroyContext();
        windowingBackend->destroyWindow(window);
        windowingBackend->shutdown();
        return EXIT_FAILURE;
    }

    // backends ready

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
    bool done = false;
    while (!done && mainWindow.isOpen()) {
        // Process events and forward to ImGui
        imguiPlatformBackend->processEvents();

        // Check if window should close
        if (windowingBackend->shouldClose(window)) {
            done = true;
        }

        // Process queued tasks from remidy
        eventLoopPtr->processQueuedTasks();

        // CRITICAL: Make our GL context current BEFORE any ImGui/GL operations
        // Plugins may have grabbed the context during event processing or callbacks
        windowingBackend->makeContextCurrent(window);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
#ifdef GL_DRAW_FRAMEBUFFER
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
#endif
#ifdef GL_READ_FRAMEBUFFER
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
#endif
#ifdef GL_BACK
        glDrawBuffer(GL_BACK);
        glReadBuffer(GL_BACK);
#endif

        // Start the Dear ImGui frame
        imguiRenderer->newFrame();
        imguiPlatformBackend->newFrame();
        ImGui::NewFrame();

        // Render main window (pass the raw window pointer for DPI calculations)
        #ifdef USE_SDL2_BACKEND
            mainWindow.render(window->sdlWindow);
        #elif defined(USE_SDL3_BACKEND)
            mainWindow.render(window->sdlWindow);
        #elif defined(USE_GLFW_BACKEND)
            mainWindow.render(window->glfwWindow);
        #endif

        // Update (handles dialogs)
        mainWindow.update();

        // Rendering
        ImGui::Render();

        // Reassert before we execute GL commands in case a plugin reclaimed it mid-frame
        windowingBackend->makeContextCurrent(window);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
#ifdef GL_DRAW_FRAMEBUFFER
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
#endif
#ifdef GL_READ_FRAMEBUFFER
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
#endif
#ifdef GL_BACK
        glDrawBuffer(GL_BACK);
        glReadBuffer(GL_BACK);
#endif

        int display_w, display_h;
        windowingBackend->getDrawableSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(clear_color.x * clear_color.w, clear_color.y * clear_color.w,
                     clear_color.z * clear_color.w, clear_color.w);
        glClear(GL_COLOR_BUFFER_BIT);
        imguiRenderer->renderDrawData();

        windowingBackend->swapBuffers(window);
    }

    // Final guard: ensure audio is stopped before teardown
    uapmd::AppModel::instance().sequencer().stopAudio();

    // Cleanup
    uapmd::AppModel::instance().sequencer().stopAudio();
    imguiRenderer->shutdown();
    imguiPlatformBackend->shutdown();
    ImGui::DestroyContext();

    windowingBackend->destroyWindow(window);
    windowingBackend->shutdown();

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
