#define SDL_MAIN_HANDLED

#include "AppModel.hpp"
#include "gui/MainWindow.hpp"
#include "gui/FontLoader.hpp"
#include <ImGuiEventLoop.hpp>
#include <PlatformBackend.hpp>
#if UAPMD_HAS_CPPTRACE
#include <cpptrace/from_current.hpp>
#endif
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_opengles2.h>
#include <emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/val.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl3.h>
#include <GLES3/gl3.h>
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string_view>
#include <sys/stat.h>
#include <uapmd-data/uapmd-data.hpp>

struct WasmContext {
    SDL_Window* window = nullptr;
    SDL_GLContext glContext = nullptr;
    ImGuiContext* imguiContext = nullptr;
    uapmd::gui::ImGuiEventLoop* eventLoop = nullptr;
    uapmd::gui::MainWindow* mainWindow = nullptr;
    bool quit = false;
};

static WasmContext g_ctx;

extern "C" void uapmd_web_storage_ready(int ok);

EM_JS(void, uapmd_init_browser_storage, (), {
    if (typeof FS === 'undefined' || typeof IDBFS === 'undefined') {
        console.error('[uapmd] Browser storage unavailable: FS or IDBFS is missing.');
        _uapmd_web_storage_ready(0);
        return;
    }
    try {
        if (!FS.analyzePath('/browser').exists)
            FS.mkdir('/browser');
        if (!FS.analyzePath('/browser/uploads').exists)
            FS.mkdir('/browser/uploads');
        if (!FS.analyzePath('/browser/remidy-tooling').exists)
            FS.mkdir('/browser/remidy-tooling');
        try {
            FS.mount(IDBFS, {}, '/browser');
        } catch (e) {
            if (!String(e).includes('already mounted'))
                throw e;
        }
        FS.syncfs(true, function(err) {
            if (err)
                console.error('[uapmd] Failed to load browser filesystem:', err);
            else
                console.log('[uapmd] Browser filesystem ready.');
            _uapmd_web_storage_ready(err ? 0 : 1);
        });
    } catch (e) {
        console.error('[uapmd] Failed to initialize browser filesystem:', e);
        _uapmd_web_storage_ready(0);
    }
});

extern "C" void uapmd_debug_import_audio(const char* path);

namespace {

std::string urlDecode(const std::string& input) {
    std::string output;
    output.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        char c = input[i];
        if (c == '+') {
            output.push_back(' ');
        } else if (c == '%' && i + 2 < input.size()) {
            std::string hex = input.substr(i + 1, 2);
            char decoded = static_cast<char>(std::strtol(hex.c_str(), nullptr, 16));
            output.push_back(decoded);
            i += 2;
        } else {
            output.push_back(c);
        }
    }
    return output;
}

std::optional<std::string> getQueryParam(const std::string& key) {
    using emscripten::val;
    val location = val::global("window")["location"];
    std::string search = location["search"].as<std::string>();
    if (search.empty())
        return std::nullopt;
    size_t pos = (search[0] == '?') ? 1 : 0;
    while (pos < search.size()) {
        size_t amp = search.find('&', pos);
        std::string_view token(search.c_str() + pos,
                               (amp == std::string::npos) ? search.size() - pos : amp - pos);
        size_t eq = token.find('=');
        std::string name = std::string(token.substr(0, eq));
        std::string value = (eq == std::string::npos) ? "" : std::string(token.substr(eq + 1));
        if (name == key)
            return urlDecode(value);
        if (amp == std::string::npos)
            break;
        pos = amp + 1;
    }
    return std::nullopt;
}

void ensureDirectory(const char* path) {
    mkdir(path, 0777);
}

void autoImportOnLoad(const char* path) {
    std::cout << "[autoImport] downloaded '" << path << "'" << std::endl;
    uapmd_debug_import_audio(path);
}

void autoImportOnError(int status, const char* path) {
    std::cout << "[autoImport] download failed (" << status << ") for '" << path << "'" << std::endl;
}

void maybeScheduleAutoImport() {
    auto param = getQueryParam("autoImport");
    if (!param || param->empty())
        return;

    const char* target = "/browser/uploads/auto-import.wav";
    ensureDirectory("/browser");
    ensureDirectory("/browser/uploads");
    std::cout << "[autoImport] fetching '" << *param << "' -> " << target << std::endl;
    int status = emscripten_wget(param->c_str(), target);
    if (status == 0) {
        autoImportOnLoad(target);
    } else {
        autoImportOnError(status, target);
    }
}

} // namespace

static void mainLoopIteration() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        ImGui_ImplSDL3_ProcessEvent(&event);
        if (event.type == SDL_EVENT_QUIT) {
            g_ctx.quit = true;
        }
    }

    ImGui_ImplSDL3_NewFrame();
    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();

    if (g_ctx.mainWindow && g_ctx.mainWindow->isOpen()) {
        g_ctx.mainWindow->render(g_ctx.window);
        g_ctx.mainWindow->update();
    } else {
        ImGui::Begin("uapmd-app - WebAssembly");
        ImGui::Text("Port in progress");
        ImGui::End();
    }

    ImGui::Render();
    ImDrawData* drawData = ImGui::GetDrawData();

    SDL_GL_MakeCurrent(g_ctx.window, g_ctx.glContext);
    int drawableWidth = 0;
    int drawableHeight = 0;
    if (!SDL_GetWindowSizeInPixels(g_ctx.window, &drawableWidth, &drawableHeight)) {
        SDL_GetWindowSize(g_ctx.window, &drawableWidth, &drawableHeight);
    }
    if (drawableWidth <= 0) {
        drawableWidth = 1;
    }
    if (drawableHeight <= 0) {
        drawableHeight = 1;
    }
    glViewport(0, 0, drawableWidth, drawableHeight);
    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(drawData);
    SDL_GL_SwapWindow(g_ctx.window);

    if (g_ctx.eventLoop) {
        g_ctx.eventLoop->processQueuedTasks();
    }

    if (g_ctx.quit) {
        emscripten_cancel_main_loop();
    }
}

static void cleanup() {
    delete g_ctx.mainWindow;
    g_ctx.mainWindow = nullptr;

    delete g_ctx.eventLoop;
    g_ctx.eventLoop = nullptr;

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();

    if (g_ctx.imguiContext) {
        ImGui::DestroyContext(g_ctx.imguiContext);
        g_ctx.imguiContext = nullptr;
    }

    if (g_ctx.glContext) {
        SDL_GL_DestroyContext(g_ctx.glContext);
        g_ctx.glContext = nullptr;
    }

    if (g_ctx.window) {
        SDL_DestroyWindow(g_ctx.window);
        g_ctx.window = nullptr;
    }

    SDL_Quit();
}

static int runWasmApp() {
    SDL_SetMainReady();
    SDL_SetHint(SDL_HINT_EMSCRIPTEN_CANVAS_SELECTOR, "#canvas");
    SDL_SetHint(SDL_HINT_EMSCRIPTEN_KEYBOARD_ELEMENT, "#canvas");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << std::endl;
        return EXIT_FAILURE;
    }

    auto setGlAttribute = [](SDL_GLAttr attr, int value, const char* name) {
        if (!SDL_GL_SetAttribute(attr, value)) {
            std::cerr << "SDL_GL_SetAttribute(" << name << ") failed: " << SDL_GetError() << std::endl;
        }
    };
    setGlAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3, "MAJOR_VERSION");
    setGlAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0, "MINOR_VERSION");
    setGlAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES, "PROFILE_MASK");

        g_ctx.window = SDL_CreateWindow("uapmd-app (WebAssembly)",
                                        1024,
                                        768,
                                        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
        if (g_ctx.window) {
            SDL_SetWindowPosition(g_ctx.window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        }
    if (!g_ctx.window) {
        std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << std::endl;
        SDL_Quit();
        return EXIT_FAILURE;
    }

    g_ctx.glContext = SDL_GL_CreateContext(g_ctx.window);
    if (!g_ctx.glContext) {
        std::cerr << "SDL_GL_CreateContext failed: " << SDL_GetError() << std::endl;
        SDL_DestroyWindow(g_ctx.window);
        SDL_Quit();
        return EXIT_FAILURE;
    }

    IMGUI_CHECKVERSION();
    g_ctx.imguiContext = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;
    ImGui::LoadIniSettingsFromMemory("", 0);
    ImGui::StyleColorsDark();
    uapmd::gui::ensureApplicationFont();

    ImGui_ImplSDL3_InitForOpenGL(g_ctx.window, g_ctx.glContext);
    ImGui_ImplOpenGL3_Init("#version 300 es");

    g_ctx.eventLoop = new uapmd::gui::ImGuiEventLoop();
    remidy::setEventLoop(g_ctx.eventLoop);
    remidy::EventLoop::initializeOnUIThread();

    uapmd::AppModel::instantiate();
    uapmd::gui::GuiDefaults defaults;
    g_ctx.mainWindow = new uapmd::gui::MainWindow(defaults);
    uapmd::AppModel::instance().notifyUiReady();
    maybeScheduleAutoImport();
    uapmd::AppModel::instance().setAudioEngineEnabled(false);

    uapmd_init_browser_storage();
    emscripten_set_main_loop(mainLoopIteration, 0, 1);
    cleanup();
    uapmd::AppModel::cleanupInstance();
    return EXIT_SUCCESS;
}

extern "C" {
EMSCRIPTEN_KEEPALIVE
void uapmd_debug_import_audio(const char* path) {
    if (!path || !*path) {
        std::cout << "[wasm-debug] import_audio: empty path\n";
        return;
    }

    const std::string filepath(path);
    auto& appModel = uapmd::AppModel::instance();
    auto tracks = appModel.getTimelineTracks();
    if (tracks.empty()) {
        auto trackIndex = appModel.addTrack();
        if (trackIndex < 0) {
            std::cout << "[wasm-debug] import_audio: failed to create track\n";
            return;
        }
        std::cout << "[wasm-debug] import_audio: created track " << trackIndex << "\n";
    }

    auto reader = uapmd::createAudioFileReaderFromPath(filepath);
    if (!reader) {
        std::cout << "[wasm-debug] import_audio: failed to open '" << filepath << "'\n";
        return;
    }

    uapmd::TimelinePosition position;
    position.samples = 0;
    position.legacy_beats = 0.0;
    auto result = appModel.addClipToTrack(0, position, std::move(reader), filepath);
    if (!result.success) {
        std::cout << "[wasm-debug] import_audio: engine error '" << result.error << "'\n";
        return;
    }

    std::cout << "[wasm-debug] import_audio: clip " << result.clipId << " added\n";
}

EMSCRIPTEN_KEEPALIVE
void uapmd_debug_load_project(const char* path) {
    if (!path || !*path) {
        std::cout << "[wasm-debug] load_project: empty path\n";
        return;
    }
    auto& appModel = uapmd::AppModel::instance();
    auto result = appModel.loadProjectFromResolvedPath(std::filesystem::path(path));
    if (!result.success)
        std::cout << "[wasm-debug] load_project: failed: " << result.error << "\n";
    else
        std::cout << "[wasm-debug] load_project: OK\n";
}

EMSCRIPTEN_KEEPALIVE
void uapmd_debug_enable_audio_engine(int enable) {
    auto& appModel = uapmd::AppModel::instance();
    appModel.setAudioEngineEnabled(enable != 0);
    std::cout << "[wasm-debug] audio_engine: " << (enable ? "ON" : "OFF") << "\n";
}

EMSCRIPTEN_KEEPALIVE
void uapmd_debug_start_playback() {
    auto& appModel = uapmd::AppModel::instance();
    appModel.transport().play();
    std::cout << "[wasm-debug] start_playback called, isPlaying="
              << appModel.transport().isPlaying() << "\n";
}

EMSCRIPTEN_KEEPALIVE
double uapmd_debug_get_playback_position_samples() {
    auto& appModel = uapmd::AppModel::instance();
    return static_cast<double>(appModel.timeline().playheadPosition.samples);
}

EMSCRIPTEN_KEEPALIVE
void uapmd_web_storage_ready(int ok) {
    if (!ok) {
        std::cerr << "[uapmd] Browser storage sync failed; continuing without persistent cache." << std::endl;
        return;
    }
    uapmd::AppModel::instance().notifyPersistentStorageReady();
}
}

int main() {
#if UAPMD_HAS_CPPTRACE
    CPPTRACE_TRY {
        return runWasmApp();
    } CPPTRACE_CATCH(const std::exception& e) {
        (void)e;
        cpptrace::from_current_exception().print();
        return EXIT_FAILURE;
    }
#else
    return runWasmApp();
#endif
}
