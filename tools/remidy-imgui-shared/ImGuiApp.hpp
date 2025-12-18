#pragma once

#include "PlatformBackend.hpp"
#include "ImGuiEventLoop.hpp"
#include <imgui.h>
#include <memory>
#include <functional>

// Platform-specific GL headers
#if defined(__APPLE__)
    #include <OpenGL/gl3.h>
#elif defined(_WIN32)
    #include <GL/gl.h>
#else
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
#if defined(__linux__) || defined(_WIN32)
#define REMIDY_SKIP_GL_FRAMEBUFFER_BIND 1
#endif

namespace uapmd::gui {

/**
 * Configuration for ImGui application
 */
struct ImGuiAppConfig {
    const char* windowTitle = "ImGui Application";
    int windowWidth = 800;
    int windowHeight = 600;
    ImVec4 clearColor = ImVec4(0.12f, 0.12f, 0.12f, 1.00f);
    bool enableKeyboard = true;
    float dpiScale = 1.0f; // 0 = auto-detect
};

/**
 * Shared ImGui application runner.
 * Handles windowing backend initialization, ImGui setup, and main render loop.
 * It used to be shared by remidy-plugin-host, uapmd-service, and uapmd-app to avoid code duplication.
 */
class ImGuiApp {
public:
    /**
     * Run the ImGui application with the given callbacks.
     *
     * @param config Application configuration
     * @param onInit Called after ImGui initialization, before main loop. Return false to abort.
     * @param onFrame Called each frame to render UI. Return false to exit.
     * @param onShutdown Called before cleanup.
     * @return Exit code
     */
    static int run(
        const ImGuiAppConfig& config,
        std::function<bool(ImGuiEventLoop*)> onInit,
        std::function<bool(ImGuiEventLoop*, WindowHandle*)> onFrame,
        std::function<void()> onShutdown = nullptr
    ) {
        // Create windowing backend
        auto windowingBackend = WindowingBackend::create();
        if (!windowingBackend) {
            return EXIT_FAILURE;
        }

        if (!windowingBackend->initialize()) {
            return EXIT_FAILURE;
        }

        // Create window
        WindowHandle* window = windowingBackend->createWindow(
            config.windowTitle, config.windowWidth, config.windowHeight);
        if (!window) {
            windowingBackend->shutdown();
            return EXIT_FAILURE;
        }

        // Setup Dear ImGui
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        if (config.enableKeyboard) {
            io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        }
        ImGui::StyleColorsDark();

        // Apply DPI scaling
        float dpiScale = config.dpiScale;
        if (dpiScale <= 0.0f) {
            dpiScale = 1.0f; // TODO: platform-specific DPI detection
        }
        if (dpiScale > 1.1f && dpiScale <= 3.0f) {
            ImGuiStyle& style = ImGui::GetStyle();
            style.ScaleAllSizes(dpiScale);
        }

        // Create ImGui backends
        auto imguiPlatformBackend = ImGuiPlatformBackend::create(window);
        auto imguiRenderer = ImGuiRenderer::create();

        if (!imguiPlatformBackend || !imguiRenderer) {
            ImGui::DestroyContext();
            windowingBackend->destroyWindow(window);
            windowingBackend->shutdown();
            return EXIT_FAILURE;
        }

        if (!imguiPlatformBackend->initialize(window) || !imguiRenderer->initialize(window)) {
            ImGui::DestroyContext();
            windowingBackend->destroyWindow(window);
            windowingBackend->shutdown();
            return EXIT_FAILURE;
        }

        // Initialize Remidy event loop
        auto eventLoop = std::make_unique<ImGuiEventLoop>();
        auto* eventLoopPtr = eventLoop.get();
        remidy::setEventLoop(eventLoop.release());
        remidy::EventLoop::initializeOnUIThread();

        // User initialization
        if (!onInit(eventLoopPtr)) {
            cleanup(imguiRenderer, imguiPlatformBackend, windowingBackend, window);
            return EXIT_FAILURE;
        }

        // Main loop
        bool done = false;
        while (!done) {
            // Process events
            imguiPlatformBackend->processEvents();

            if (windowingBackend->shouldClose(window)) {
                done = true;
                continue;
            }

            // Process queued tasks from remidy
            eventLoopPtr->processQueuedTasks();

            // Ensure GL context is current before ImGui/GL operations
            bindMainFramebuffer(windowingBackend, window);

            // Start ImGui frame
            imguiRenderer->newFrame();
            imguiPlatformBackend->newFrame();
            ImGui::NewFrame();

            // User frame rendering
            if (!onFrame(eventLoopPtr, window)) {
                done = true;
            }

            // Render ImGui
            ImGui::Render();

            // Reassert GL context before executing GL commands
            bindMainFramebuffer(windowingBackend, window);

            // Render to screen
            int displayW, displayH;
            windowingBackend->getDrawableSize(window, &displayW, &displayH);
            glViewport(0, 0, displayW, displayH);
            glClearColor(config.clearColor.x * config.clearColor.w,
                        config.clearColor.y * config.clearColor.w,
                        config.clearColor.z * config.clearColor.w,
                        config.clearColor.w);
            glClear(GL_COLOR_BUFFER_BIT);
            imguiRenderer->renderDrawData();

            windowingBackend->swapBuffers(window);
        }

        // User shutdown
        if (onShutdown) {
            onShutdown();
        }

        // Cleanup
        cleanup(imguiRenderer, imguiPlatformBackend, windowingBackend, window);

        return EXIT_SUCCESS;
    }

private:
    static void bindMainFramebuffer(std::unique_ptr<WindowingBackend>& backend, WindowHandle* window) {
        // CRITICAL: Make GL context current BEFORE any ImGui/GL operations
        // Plugins may have grabbed the context during event processing or callbacks
        backend->makeContextCurrent(window);

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
    }

    static void cleanup(
        std::unique_ptr<ImGuiRenderer>& renderer,
        std::unique_ptr<ImGuiPlatformBackend>& platform,
        std::unique_ptr<WindowingBackend>& windowing,
        WindowHandle* window
    ) {
        renderer->shutdown();
        platform->shutdown();
        ImGui::DestroyContext();
        windowing->destroyWindow(window);
        windowing->shutdown();
    }
};

} // namespace uapmd::gui
