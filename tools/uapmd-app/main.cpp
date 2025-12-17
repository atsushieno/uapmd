#include "AppModel.hpp"
#include "gui/MainWindow.hpp"
#include <ImGuiEventLoop.hpp>
#include <PlatformBackend.hpp>
#include <cpptrace/from_current.hpp>
#include <algorithm>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include <imgui.h>

#ifdef USE_GLFW_BACKEND
    #include <GLFW/glfw3.h>
#endif

// Ensure GL prototypes for framebuffer functions on Linux
#if defined(__APPLE__)
    #include <OpenGL/gl3.h>
#elif defined(_WIN32)
    // Windows: only include basic OpenGL 1.1 headers
    // Modern GL functions would need GLEW/GLAD, but we skip those on Windows
    #include <GL/gl.h>
#else
    // Linux: try to get GL3 headers with extensions
    #if defined(__has_include)
        #if __has_include(<GL/gl3.h>)
            #include <GL/gl3.h>
        #else
            #ifndef GL_GLEXT_PROTOTYPES
            #define GL_GLEXT_PROTOTYPES 1
            #endif
            #include <GL/gl.h>
            #include <GL/glext.h>
        #endif
    #else
        #ifndef GL_GLEXT_PROTOTYPES
        #define GL_GLEXT_PROTOTYPES 1
        #endif
        #include <GL/gl.h>
        #include <GL/glext.h>
    #endif
#endif

// On Linux/Windows without a GL loader, some GL3 prototypes may be absent
// Windows <GL/gl.h> only provides OpenGL 1.1, needs GLEW/GLAD for modern functions
#if defined(__linux__) || defined(_WIN32)
#define REMIDY_SKIP_GL_FRAMEBUFFER_BIND 1
#endif

int runMain(int argc, char** argv) {
    std::vector<std::string> args;
    args.reserve(static_cast<size_t>(std::max(argc - 1, 0)));
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }

    enum class Mode {
        Gui,
        Headless
    };

    std::optional<Mode> requestedMode;
    std::vector<std::string> positional;
    positional.reserve(args.size());
    for (const auto& arg : args) {
        /*if (arg == "--shell" || arg == "--cli" || arg == "--no-gui" || arg == "--headless") {
            requestedMode = Mode::Headless;
            continue;
        }
        if (arg == "--gui") {
            requestedMode = Mode::Gui;
            continue;
        }*/
        positional.push_back(arg);
    }

    bool guiAvailable = true;

    bool runGui = guiAvailable;
    if (requestedMode.has_value()) {
        if (*requestedMode == Mode::Gui) {
            if (!guiAvailable) {
                std::cerr << "uapmd-app built without GUI support; continuing in headless mode" << std::endl;
                runGui = false;
            } else {
                runGui = true;
            }
        } else {
            runGui = false;
        }
    }

    if (!runGui) {
        std::cerr << "uapmd-app can only run in GUI mode" << std::endl;
        return EXIT_FAILURE;
    }

    // Create windowing backend with priority: SDL3 > SDL2 > GLFW
    auto windowingBackend = uapmd::gui::WindowingBackend::create();
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
    uapmd::gui::WindowHandle* window = windowingBackend->createWindow("UAPMD", 800, 800);
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
    auto imguiPlatformBackend = uapmd::gui::ImGuiPlatformBackend::create(window);
    auto imguiRenderer = uapmd::gui::ImGuiRenderer::create();

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

    // Prepare GUI defaults from command line arguments
    uapmd::gui::GuiDefaults defaults;
    if (!positional.empty()) defaults.pluginName = positional[0];
    if (positional.size() > 1) defaults.formatName = positional[1];
    if (positional.size() > 2) defaults.apiName = positional[2];
    if (positional.size() > 3) defaults.deviceName = positional[3];

    // Create main window controller
    uapmd::gui::MainWindow mainWindow(defaults);

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
        #if !defined(REMIDY_SKIP_GL_FRAMEBUFFER_BIND)
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        #endif
#ifdef GL_DRAW_FRAMEBUFFER
        #if !defined(REMIDY_SKIP_GL_FRAMEBUFFER_BIND)
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        #endif
#endif
#ifdef GL_READ_FRAMEBUFFER
        #if !defined(REMIDY_SKIP_GL_FRAMEBUFFER_BIND)
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        #endif
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
        #if !defined(REMIDY_SKIP_GL_FRAMEBUFFER_BIND)
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        #endif
#ifdef GL_DRAW_FRAMEBUFFER
        #if !defined(REMIDY_SKIP_GL_FRAMEBUFFER_BIND)
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        #endif
#endif
#ifdef GL_READ_FRAMEBUFFER
        #if !defined(REMIDY_SKIP_GL_FRAMEBUFFER_BIND)
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        #endif
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
        std::cerr << "Exception in uapmd-app: " << e.what() << std::endl;
        cpptrace::from_current_exception().print();
        return EXIT_FAILURE;
    }
}
