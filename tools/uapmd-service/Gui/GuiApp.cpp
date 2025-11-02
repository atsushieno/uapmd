#include "GuiApp.hpp"

#include <iostream>
#include <utility>

#include <imgui.h>
#include <remidy/remidy.hpp>

#include "ImGuiEventLoop.hpp"
#include "PlatformBackend.hpp"

#ifdef USE_GLFW_BACKEND
    #include <GLFW/glfw3.h>
#endif

#if defined(__APPLE__)
    #include <OpenGL/gl3.h>
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

namespace uapmd::service::gui {

using uapmd::VirtualMidiDeviceController;

// On Linux without a GL loader, some GL3 prototypes may be absent
#if defined(__linux__)
#define REMIDY_SKIP_GL_FRAMEBUFFER_BIND 1
#endif

int GuiApp::run(int argc, const char** /*argv*/, GuiDefaults defaults) {
    (void) argc;

    auto windowingBackend = uapmd::gui::WindowingBackend::create();
    if (!windowingBackend) {
        std::cerr << "uapmd-service: no windowing backend available" << std::endl;
        return EXIT_FAILURE;
    }

    if (!windowingBackend->initialize()) {
        std::cerr << "uapmd-service: failed to initialize " << windowingBackend->getName() << " backend" << std::endl;
        return EXIT_FAILURE;
    }

    uapmd::gui::WindowHandle* window = windowingBackend->createWindow("uapmd-service", 960, 720);
    if (!window) {
        std::cerr << "uapmd-service: failed to create window" << std::endl;
        windowingBackend->shutdown();
        return EXIT_FAILURE;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();

    auto imguiPlatformBackend = uapmd::gui::ImGuiPlatformBackend::create(window);
    auto imguiRenderer = uapmd::gui::ImGuiRenderer::create();
    if (!imguiPlatformBackend || !imguiRenderer) {
        std::cerr << "uapmd-service: failed to create ImGui backends" << std::endl;
        ImGui::DestroyContext();
        windowingBackend->destroyWindow(window);
        windowingBackend->shutdown();
        return EXIT_FAILURE;
    }

    if (!imguiPlatformBackend->initialize(window) || !imguiRenderer->initialize(window)) {
        std::cerr << "uapmd-service: failed to initialize ImGui backends" << std::endl;
        imguiPlatformBackend->shutdown();
        imguiRenderer->shutdown();
        ImGui::DestroyContext();
        windowingBackend->destroyWindow(window);
        windowingBackend->shutdown();
        return EXIT_FAILURE;
    }

    auto eventLoop = std::make_unique<ImGuiEventLoop>();
    auto* eventLoopPtr = eventLoop.get();
    remidy::setEventLoop(eventLoop.release());
    remidy::EventLoop::initializeOnUIThread();

    VirtualMidiDeviceController controller;
    MainWindow mainWindow(controller, std::move(defaults));

    ImVec4 clearColor = ImVec4(0.12f, 0.12f, 0.12f, 1.00f);
    bool done = false;
    while (!done && mainWindow.isOpen()) {
        imguiPlatformBackend->processEvents();
        if (windowingBackend->shouldClose(window)) {
            done = true;
            mainWindow.close();
        }

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

        imguiRenderer->newFrame();
        imguiPlatformBackend->newFrame();
        ImGui::NewFrame();

        mainWindow.render();
        mainWindow.update();

        ImGui::Render();

        // Reassert before executing GL commands; some plugins grab it again mid-frame
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

        int displayW = 0;
        int displayH = 0;
        windowingBackend->getDrawableSize(window, &displayW, &displayH);
        glViewport(0, 0, displayW, displayH);
        glClearColor(clearColor.x * clearColor.w, clearColor.y * clearColor.w,
                     clearColor.z * clearColor.w, clearColor.w);
        glClear(GL_COLOR_BUFFER_BIT);
        imguiRenderer->renderDrawData();
        windowingBackend->swapBuffers(window);
    }

    mainWindow.shutdown();

    imguiRenderer->shutdown();
    imguiPlatformBackend->shutdown();
    ImGui::DestroyContext();
    windowingBackend->destroyWindow(window);
    windowingBackend->shutdown();

    return EXIT_SUCCESS;
}

} // namespace uapmd::service::gui
