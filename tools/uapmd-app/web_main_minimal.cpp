#include <SDL3/SDL.h>
#include <SDL3/SDL_opengles2.h>
#include <emscripten.h>
#include <emscripten/html5.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl3.h>
#include <GLES3/gl3.h>
#include <iostream>

struct WasmContext {
    SDL_Window* window = nullptr;
    SDL_GLContext glContext = nullptr;
    bool quit = false;
};

static WasmContext g_ctx;

static void loop() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) g_ctx.quit = true;
    }

    ImGui_ImplSDL3_NewFrame();
    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();
    ImGui::Begin("uapmd-app");
    ImGui::Text("Minimal stub running inside WebAssembly");
    ImGui::End();
    ImGui::Render();

    SDL_GL_MakeCurrent(g_ctx.window, g_ctx.glContext);
    glViewport(0, 0, 0, 0);
    glClearColor(0.2f, 0.2f, 0.2f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    SDL_GL_SwapWindow(g_ctx.window);

    if (g_ctx.quit) {
        emscripten_cancel_main_loop();
    }
}

int main() {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        return EXIT_FAILURE;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);

    g_ctx.window = SDL_CreateWindow("uapmd-app", 1024, 768,
                                    SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (g_ctx.window) {
        SDL_SetWindowPosition(g_ctx.window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
    if (!g_ctx.window) {
        SDL_Quit();
        return EXIT_FAILURE;
    }

    g_ctx.glContext = SDL_GL_CreateContext(g_ctx.window);
    if (!g_ctx.glContext) {
        SDL_DestroyWindow(g_ctx.window);
        SDL_Quit();
        return EXIT_FAILURE;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplSDL3_InitForOpenGL(g_ctx.window, g_ctx.glContext);
    ImGui_ImplOpenGL3_Init("#version 300 es");

    emscripten_set_main_loop(loop, 0, 1);

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(g_ctx.glContext);
    SDL_DestroyWindow(g_ctx.window);
    SDL_Quit();

    return EXIT_SUCCESS;
}
