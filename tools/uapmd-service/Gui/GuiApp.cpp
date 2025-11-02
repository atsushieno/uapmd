#include "GuiApp.hpp"

#include <iostream>
#include <utility>

#include <imgui.h>
#include <remidy/remidy.hpp>

#include "ImGuiEventLoop.hpp"
#include "PlatformBackend.hpp"

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

namespace uapmd::service::gui {

using uapmd::VirtualMidiDeviceController;

int GuiApp::run(int argc, const char** /*argv*/, GuiDefaults defaults) {
    (void) argc;

    auto windowingBackend = WindowingBackend::create();
    if (!windowingBackend) {
        std::cerr << "uapmd-service: no windowing backend available" << std::endl;
        return EXIT_FAILURE;
    }

    if (!windowingBackend->initialize()) {
        std::cerr << "uapmd-service: failed to initialize " << windowingBackend->getName() << " backend" << std::endl;
        return EXIT_FAILURE;
    }

    auto* window = windowingBackend->createWindow("uapmd-service", 960, 720);
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

    auto imguiPlatformBackend = ImGuiPlatformBackend::create(window);
    auto imguiRenderer = ImGuiRenderer::create();
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

        imguiRenderer->newFrame();
        imguiPlatformBackend->newFrame();
        ImGui::NewFrame();

        mainWindow.render();
        mainWindow.update();

        ImGui::Render();

        // Reassert before executing GL commands; some plugins grab it again mid-frame
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
