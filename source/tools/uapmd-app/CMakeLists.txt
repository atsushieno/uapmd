set(UAPMD_BUILDING_WASM OFF)
if(UAPMD_TARGET_WASM)
    if(NOT EMSCRIPTEN)
        message(FATAL_ERROR "UAPMD_TARGET_WASM=ON requires configuring with the Emscripten toolchain (use emcmake).")
    endif()
    set(UAPMD_BUILDING_WASM ON)
    option(UAPMD_WASM_USE_VCPKG_SDL3
        "Use vcpkg to provide SDL3 for WASM builds (OFF: Emscripten port via -s USE_SDL=3; ON: vcpkg SDL3::SDL3)"
        OFF)
    add_compile_definitions(EMSCRIPTEN DISABLE_VST3 DISABLE_AU DISABLE_LV2 DISABLE_CLAP)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} \
        -s WASM=1 \
        -s ALLOW_MEMORY_GROWTH=1 \
        -s MAXIMUM_MEMORY=2GB \
        -s USE_WEBGL2=1 \
        -s FULL_ES3=1 \
        -s MIN_WEBGL_VERSION=2 \
        -s DISABLE_EXCEPTION_CATCHING=0 \
        -s NO_EXIT_RUNTIME=1 \
        -s ASSERTIONS=1 \
        -s SHARED_MEMORY=1 \
        -s WASM_WORKERS=1 \
        -s AUDIO_WORKLET=1 \
        -pthread \
        -s PTHREAD_POOL_SIZE=8 \
        -s EXPORTED_RUNTIME_METHODS=['ccall','cwrap','getValue','setValue','UTF8ToString','stringToUTF8','allocateUTF8','FS'] \
        -s EXPORTED_FUNCTIONS=['_main','_uapmd_mcp_call','_uapmd_eval','_uapmd_debug_import_audio','_uapmd_debug_load_project','_uapmd_debug_enable_audio_engine','_uapmd_debug_start_playback','_uapmd_debug_get_playback_position_samples','_uapmd_web_storage_ready','_uapmd_webclap_on_worklet_message','_uapmd_webclap_on_worklet_state_response','_uapmd_webclap_on_worklet_state_load_complete']")
    if(NOT UAPMD_WASM_USE_VCPKG_SDL3)
        # Emscripten port: the linker flag activates the bundled SDL3 port at link time.
        string(APPEND CMAKE_EXE_LINKER_FLAGS " -s USE_SDL=3")
    endif()
endif()

# MCP: cpp-httplib for embedded HTTP server mode (desktop only)
if(NOT UAPMD_BUILDING_WASM AND NOT IOS AND NOT ANDROID)
    CPMAddPackage(
        NAME cpp-httplib
        GIT_TAG v0.18.1
        GIT_REPOSITORY https://github.com/yhirose/cpp-httplib
        DOWNLOAD_ONLY YES
    )
endif()

# MCP: IXWebSocket for outbound WebSocket client mode (all platforms except Wasm —
# Wasm uses the browser's native WebSocket via a future Emscripten binding path)
if(NOT UAPMD_BUILDING_WASM)
    CPMAddPackage(
        NAME ixwebsocket
        GIT_TAG v11.4.6
        GIT_REPOSITORY https://github.com/machinezone/IXWebSocket
        OPTIONS
            "USE_TLS OFF"
            "USE_ZLIB OFF"
            "IXWEBSOCKET_INSTALL OFF"
    )
endif()

# Common application sources
set(UAPMD_APP_COMMON_SOURCE_FILES
        main_common.cpp
        AppModel.cpp
        UapmdJSRuntime.cpp
        gui/MainWindow.cpp
        gui/PluginList.cpp
        gui/InstanceDetails.cpp
        gui/PluginSelector.cpp
        gui/PluginGraphEditor.cpp
        gui/ParameterList.cpp
        gui/TrackList.cpp
        gui/AudioDeviceSettings.cpp
        gui/AudioImportWindow.cpp
        gui/ExporterWindow.cpp
        gui/ScriptEditor.cpp
        gui/SpectrumAnalyzer.cpp
        gui/SequenceEditor.cpp
        gui/TimelineEditor.cpp
        gui/ClipPreview.cpp
        gui/AudioEventListEditor.cpp
        gui/MidiDumpWindow.cpp
        gui/PianoRollEditor.cpp
        gui/FontLoader.cpp
        Configuration/AudioDeviceConfiguration.cpp
        Configuration/VirtualMidiDeviceConfiguration.cpp
)

list(APPEND UAPMD_APP_COMMON_SOURCE_FILES mcp/McpServer.cpp)

# Platform-specific entrypoints
if(UAPMD_BUILDING_WASM)
    set(UAPMD_APP_SOURCE_FILES
            ${UAPMD_APP_COMMON_SOURCE_FILES}
            web_main.cpp
    )
    set(_UAPMD_WASM_WEB_SRC "${CMAKE_CURRENT_SOURCE_DIR}/web")
    set(_UAPMD_WASM_WEB_DST "${CMAKE_CURRENT_BINARY_DIR}")
    set(_UAPMD_WASM_WEB_RUNTIME_FILES
            "index.html"
            "coop-coep-sw.js"
            "audioworklet-env-fix.js"
            "uapmd-webclap-worklet.js"
    )
    set(_UAPMD_WASM_WEB_STAMP "${_UAPMD_WASM_WEB_DST}/.web-assets.stamp")
    set(_UAPMD_WASM_WEB_SOURCE_FILES)
    foreach(_web_file IN LISTS _UAPMD_WASM_WEB_RUNTIME_FILES)
        list(APPEND _UAPMD_WASM_WEB_SOURCE_FILES "${_UAPMD_WASM_WEB_SRC}/${_web_file}")
    endforeach()
    add_custom_command(
            OUTPUT "${_UAPMD_WASM_WEB_STAMP}"
            COMMAND ${CMAKE_COMMAND}
                -DSRC="${_UAPMD_WASM_WEB_SRC}"
                -DDST="${_UAPMD_WASM_WEB_DST}"
                -P "${UAPMD_CMAKE_DIR}/CopyWebAssets.cmake"
            COMMAND ${CMAKE_COMMAND} -E touch "${_UAPMD_WASM_WEB_STAMP}"
            DEPENDS ${_UAPMD_WASM_WEB_SOURCE_FILES}
    )
    add_custom_target(uapmd-app-web-assets ALL DEPENDS "${_UAPMD_WASM_WEB_STAMP}")
elseif(IOS)
    set(UAPMD_APP_SOURCE_FILES
            ${UAPMD_APP_COMMON_SOURCE_FILES}
            ios_main.mm
    )
elseif(NOT ANDROID)
    set(UAPMD_APP_SOURCE_FILES
            ${UAPMD_APP_COMMON_SOURCE_FILES}
            main.cpp
    )
endif()

if(WIN32 AND NOT UAPMD_BUILDING_WASM)
    list(APPEND UAPMD_APP_SOURCE_FILES ${UAPMD_RESOURCES_DIR}/uapmd.rc)
endif()

if(ANDROID)
    # All app sources compile into libuapmd-app.so; libmain.so is the thin
    # SDL3 JNI entry point (SDL3 requires System.loadLibrary("main")).
    add_library(uapmd-app SHARED ${UAPMD_APP_COMMON_SOURCE_FILES})
    set(UAPMD_COMPILE_TARGET uapmd-app)
    add_library(main SHARED android_main.cpp)

    find_package(androidaudioplugin REQUIRED CONFIG)
elseif(IOS)
    # iOS: standard app bundle; SDL3 provides UIApplicationDelegate.
    add_executable(uapmd-app MACOSX_BUNDLE ${UAPMD_APP_SOURCE_FILES})
    set(UAPMD_COMPILE_TARGET uapmd-app)
elseif(UAPMD_BUILDING_WASM)
    add_executable(uapmd-app ${UAPMD_APP_SOURCE_FILES})
    set(UAPMD_COMPILE_TARGET uapmd-app)
    add_dependencies(uapmd-app uapmd-app-web-assets)
    target_link_options(uapmd-app PRIVATE --bind -lidbfs.js)
    # Prepend the AudioWorklet environment shim so it runs before Emscripten's
    # runtime inside the AudioWorklet WASM Worker.  It backfills the missing
    # AudioWorkletGlobalScope property (Chrome bug) and provides a navigator shim
    # to prevent ReferenceErrors on the audio thread.
    target_link_options(uapmd-app PRIVATE
        "--pre-js=${CMAKE_CURRENT_SOURCE_DIR}/web/audioworklet-env-fix.js")
else()
    # Compile all sources once into a shared OBJECT library.
    # Both uapmd-app and uapmd-app-bundle link against it, avoiding duplicate
    # compilation of the same translation units on macOS.
    add_library(uapmd-app-objects OBJECT ${UAPMD_APP_SOURCE_FILES})
    set(UAPMD_COMPILE_TARGET uapmd-app-objects)
    add_executable(uapmd-app)
    if(APPLE)
        add_executable(uapmd-app-bundle MACOSX_BUNDLE)
    endif()
endif()

# Backend selection with priority: Win32+DX11 (Windows) > SDL3 > SDL2 > GLFW
set(BACKEND_FOUND FALSE)
set(BACKEND_TYPE "")
set(BACKEND_LIBRARIES "")
set(BACKEND_INCLUDE_DIRS "")

set(_UAPMD_REQUIRE_SDL3 OFF)
if(UAPMD_BUILDING_WASM OR IOS)
    set(_UAPMD_REQUIRE_SDL3 ON)
endif()

if(NOT UAPMD_BUILDING_WASM)
    # Windows-specific: Offer Win32 + DirectX 11 backend
    if(WIN32)
        option(USE_WIN32_DIRECTX11_BACKEND "Use native Win32 + DirectX 11 backend (Windows only)" ON)

        if(USE_WIN32_DIRECTX11_BACKEND)
            set(BACKEND_FOUND TRUE)
            set(BACKEND_TYPE "Win32")
            set(BACKEND_LIBRARIES "")  # No external windowing library needed
            set(DX11_LIBRARIES d3d11 dxgi d3dcompiler)
            add_compile_definitions(USE_WIN32_BACKEND USE_DIRECTX11_RENDERER)
            message(STATUS "Using Win32 + DirectX 11 backend")
        endif()
    endif()
endif()

# Provide SDL3 via vcpkg (Windows always; Emscripten when UAPMD_WASM_USE_VCPKG_SDL3=ON)
include(${UAPMD_CMAKE_DIR}/UapmdVcpkgSDL3.cmake)
if((WIN32 OR (EMSCRIPTEN AND UAPMD_WASM_USE_VCPKG_SDL3)) AND NOT BACKEND_FOUND)
    uapmd_prepare_vcpkg_sdl3()
endif()

# Emscripten port path (UAPMD_WASM_USE_VCPKG_SDL3=OFF):
# emcc manages SDL3 entirely when -sUSE_SDL=3 is a compile flag — it builds the
# port on first use and injects the include path automatically.  No find_package,
# no manual sysroot wiring, and no embuilder pre-step required.
if(UAPMD_BUILDING_WASM AND NOT UAPMD_WASM_USE_VCPKG_SDL3 AND NOT BACKEND_FOUND)
    set(BACKEND_FOUND TRUE)
    set(BACKEND_TYPE "SDL3")
    set(BACKEND_LIBRARIES "")
    add_compile_definitions(USE_SDL3_BACKEND)
    add_compile_options("-sUSE_SDL=3")
    message(STATUS "Using SDL3 backend (Emscripten port via -sUSE_SDL=3)")
endif()

# Try SDL3 via find_package (vcpkg on Windows/Emscripten-vcpkg, or system install)
if(NOT BACKEND_FOUND)
    find_package(SDL3 QUIET)
    if(SDL3_FOUND)
        set(BACKEND_FOUND TRUE)
        set(BACKEND_TYPE "SDL3")
        if(TARGET SDL3::SDL3-static)
            set(BACKEND_LIBRARIES SDL3::SDL3-static)
        elseif(TARGET SDL3::SDL3-shared)
            set(BACKEND_LIBRARIES SDL3::SDL3-shared)
        elseif(TARGET SDL3::SDL3)
            set(BACKEND_LIBRARIES SDL3::SDL3)
        else()
            set(BACKEND_LIBRARIES SDL3::SDL3)
        endif()
        if(TARGET SDL3::SDL3main)
            list(APPEND BACKEND_LIBRARIES SDL3::SDL3main)
        endif()
        add_compile_definitions(USE_SDL3_BACKEND)
        message(STATUS "Using SDL3 backend")
        if(WIN32 OR EMSCRIPTEN)
            message(STATUS "uapmd: SDL3 config dir: ${SDL3_DIR}")
            if(SDL3_VERSION)
                message(STATUS "uapmd: SDL3 version: ${SDL3_VERSION}")
            endif()
        endif()
    endif()
endif()

# iOS: SDL3 is required (Metal/SDL_Renderer path). Build from source via CPM if not found.
if(IOS AND NOT BACKEND_FOUND)
    message(STATUS "SDL3 not found for iOS; fetching from source via CPM")
    CPMAddPackage(
        NAME SDL3
        GIT_TAG release-3.2.10
        GIT_REPOSITORY https://github.com/libsdl-org/SDL
        OPTIONS
            "SDL_STATIC ON"
            "SDL_SHARED OFF"
            "SDL_TEST_LIBRARY OFF"
            "SDL_TESTS OFF"
            "SDL_EXAMPLES OFF"
    )
    if(TARGET SDL3::SDL3-static)
        set(BACKEND_FOUND TRUE)
        set(BACKEND_TYPE "SDL3")
        set(BACKEND_LIBRARIES SDL3::SDL3-static)
        add_compile_definitions(USE_SDL3_BACKEND)
        message(STATUS "Using SDL3 backend for iOS (via CPM)")
    else()
        message(FATAL_ERROR "Failed to build SDL3 from source for iOS via CPM.")
    endif()
endif()

# Require SDL3 for wasm builds (only reachable when UAPMD_WASM_USE_VCPKG_SDL3=ON and vcpkg failed)
if(_UAPMD_REQUIRE_SDL3 AND NOT BACKEND_FOUND)
    message(FATAL_ERROR
        "UAPMD_WASM_USE_VCPKG_SDL3=ON but SDL3 was not found via vcpkg.\n"
        "Ensure vcpkg can build sdl3:wasm32-emscripten and that the Emscripten "
        "toolchain is active when cmake is invoked (use emcmake).")
endif()

if(NOT _UAPMD_REQUIRE_SDL3)
    # Try SDL2 if SDL3 not found
    if(NOT BACKEND_FOUND)
        find_package(SDL2 QUIET)
        if(SDL2_FOUND)
            set(BACKEND_FOUND TRUE)
            set(BACKEND_TYPE "SDL2")
            set(BACKEND_LIBRARIES SDL2::SDL2)
            add_compile_definitions(USE_SDL2_BACKEND)
            message(STATUS "Using SDL2 backend")
        endif()
    endif()

    # Fallback to GLFW if neither SDL3 nor SDL2 found
    if(NOT BACKEND_FOUND)
        find_package(glfw3 QUIET)
        if(glfw3_FOUND)
            set(BACKEND_FOUND TRUE)
            set(BACKEND_TYPE "GLFW")
            set(BACKEND_LIBRARIES glfw)
            add_compile_definitions(USE_GLFW_BACKEND)
            message(STATUS "Using GLFW backend")
        else()
            # Add GLFW via CPM as final fallback
            message(STATUS "No system GLFW found, adding via CPM")
            CPMAddPackage(
                NAME glfw
                GIT_TAG 3.4
                GIT_REPOSITORY https://github.com/glfw/glfw
                OPTIONS
                    "GLFW_BUILD_EXAMPLES OFF"
                    "GLFW_BUILD_TESTS OFF"
                    "GLFW_BUILD_DOCS OFF"
            )
            if(glfw_ADDED)
                set(BACKEND_FOUND TRUE)
                set(BACKEND_TYPE "GLFW")
                set(BACKEND_LIBRARIES glfw)
                add_compile_definitions(USE_GLFW_BACKEND)
                message(STATUS "Using GLFW backend (via CPM)")
            else()
                if(TARGET glfw)
                    message(WARNING "GLFW CPM: glfw_ADDED not set but target glfw exists")
                    set(BACKEND_FOUND TRUE)
                    set(BACKEND_TYPE "GLFW")
                    set(BACKEND_LIBRARIES glfw)
                    add_compile_definitions(USE_GLFW_BACKEND)
                else()
                    message(WARNING "GLFW CPM failed: glfw_ADDED=${glfw_ADDED}, no glfw target")
                endif()
            endif()
        endif()
    endif()
endif()

if(NOT BACKEND_FOUND)
    message(FATAL_ERROR "No windowing backend found! SDL2/SDL3/GLFW all unavailable.")
endif()

message(STATUS "Backend configuration: TYPE=${BACKEND_TYPE}, LIBRARIES=${BACKEND_LIBRARIES}")

# ImGui and backend dependencies
if(NOT DEFINED imgui_SOURCE_DIR)
    CPMAddPackage(
            NAME imgui
            GIT_TAG v1.92.3
            GIT_REPOSITORY https://github.com/ocornut/imgui
            DOWNLOAD_ONLY YES
    )
endif()

CPMAddPackage(
    NAME imnodes
    GIT_TAG 5025ad189b6a17c931bbdb39b8a575b63adf3e91
    GIT_REPOSITORY https://github.com/0aids/imnodes
    DOWNLOAD_ONLY YES
)

# ImTimeline for SequenceEditor timeline view
CPMAddPackage(
    NAME ImTimeline
    GIT_TAG fixes
    GIT_REPOSITORY https://github.com/triplejam/ImTimeline
    DOWNLOAD_ONLY YES
)

# IconFontCppHeaders provides Fontaudio glyph definitions used in the GUI
CPMAddPackage(
    NAME IconFontCppHeaders
    GIT_TAG main
    GIT_REPOSITORY https://github.com/juliettef/IconFontCppHeaders
    DOWNLOAD_ONLY YES
)

# Fontaudio repository (contains canonical font files)
CPMAddPackage(
    NAME fontaudio
    GIT_TAG master
    GIT_REPOSITORY https://github.com/fefanto/fontaudio
    DOWNLOAD_ONLY YES
)

# Font Awesome 7.x assets
CPMAddPackage(
    NAME FontAwesome
    GIT_TAG 7.x
    GIT_REPOSITORY https://github.com/FortAwesome/Font-Awesome
    DOWNLOAD_ONLY YES
)

# ResEmbed: build-time resource/font embedding
#
# When cross-compiling for WASM, ResEmbed's Generator must run on the host ABI.
# Define a helper that locates a usable host compiler even when the active
# toolchain is Emscripten (whose wrappers live inside the emsdk sysroot).
function(uapmd_detect_host_cxx OUTPUT_VAR)
    set(_uapmd_candidate "")
    if(ARGC GREATER 1)
        set(_uapmd_candidate "${ARGV1}")
    endif()
    if(_uapmd_candidate AND NOT _uapmd_candidate STREQUAL "" AND
            NOT _uapmd_candidate MATCHES "-NOTFOUND$")
        set(${OUTPUT_VAR} "${_uapmd_candidate}" PARENT_SCOPE)
        return()
    endif()

    set(_uapmd_result "")
    set(_uapmd_known_host_dirs /usr/bin /usr/local/bin /opt/homebrew/bin)
    find_program(_uapmd_probe_host_cxx
        NAMES clang++ g++
        PATHS ${_uapmd_known_host_dirs}
        NO_DEFAULT_PATH
        NO_CMAKE_FIND_ROOT_PATH)
    if(_uapmd_probe_host_cxx AND NOT _uapmd_probe_host_cxx MATCHES "-NOTFOUND$")
        set(_uapmd_result "${_uapmd_probe_host_cxx}")
    endif()

    if(NOT _uapmd_result AND APPLE)
        execute_process(
            COMMAND xcrun --find clang++
            OUTPUT_VARIABLE _uapmd_xcrun_host_cxx
            RESULT_VARIABLE _uapmd_xcrun_status
            OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(_uapmd_xcrun_status EQUAL 0 AND EXISTS "${_uapmd_xcrun_host_cxx}")
            set(_uapmd_result "${_uapmd_xcrun_host_cxx}")
        endif()
    endif()

    if(NOT _uapmd_result)
        find_program(_uapmd_probe_host_cxx
            NAMES clang++ g++ c++
            NO_CMAKE_FIND_ROOT_PATH)
        if(_uapmd_probe_host_cxx AND NOT _uapmd_probe_host_cxx MATCHES "-NOTFOUND$")
            set(_uapmd_result "${_uapmd_probe_host_cxx}")
        endif()
    endif()

    if(NOT _uapmd_result AND WIN32)
        find_program(_uapmd_probe_host_cxx
            NAMES cl CL)
        if(_uapmd_probe_host_cxx AND NOT _uapmd_probe_host_cxx MATCHES "-NOTFOUND$")
            set(_uapmd_result "${_uapmd_probe_host_cxx}")
        endif()
    endif()

    if(_uapmd_result MATCHES "\\.emsdk/" OR _uapmd_result MATCHES "emscripten")
        set(_uapmd_result "")
    endif()

    unset(_uapmd_known_host_dirs)
    unset(_uapmd_probe_host_cxx CACHE)
    unset(_uapmd_xcrun_host_cxx)
    unset(_uapmd_xcrun_status)

    set(${OUTPUT_VAR} "${_uapmd_result}" PARENT_SCOPE)
endfunction()

if(UAPMD_BUILDING_WASM)
    uapmd_detect_host_cxx(_uapmd_host_cxx "${UAPMD_HOST_CXX_COMPILER}")
    if(_uapmd_host_cxx)
        set(UAPMD_HOST_CXX_COMPILER "${_uapmd_host_cxx}" CACHE FILEPATH
            "Host C++ compiler for build-time tools when cross-compiling" FORCE)
    endif()

    set(_uapmd_resembed_host "${RES_EMBED_HOST_CXX}")
    if(NOT _uapmd_resembed_host OR _uapmd_resembed_host STREQUAL "" OR _uapmd_resembed_host MATCHES "-NOTFOUND$")
        set(_uapmd_resembed_host "${_uapmd_host_cxx}")
        if(_uapmd_resembed_host)
            set(RES_EMBED_HOST_CXX "${_uapmd_resembed_host}" CACHE FILEPATH
                "Host C++ compiler used to build the ResEmbed Generator when cross-compiling" FORCE)
        endif()
    endif()
    if(NOT _uapmd_resembed_host OR _uapmd_resembed_host STREQUAL "" OR _uapmd_resembed_host MATCHES "-NOTFOUND$")
        message(WARNING "uapmd: UAPMD_HOST_CXX_COMPILER is unset; ResEmbed's Generator may fail. "
            "Pass -DUAPMD_HOST_CXX_COMPILER=<path> to cmake.")
    endif()
    unset(_uapmd_host_cxx)
    unset(_uapmd_resembed_host)
endif()
set(_resembed_patch "${UAPMD_REPO_ROOT}/external/patches/resembed-host-toolchain.patch")
CPMAddPackage(
    NAME ResEmbed
    GIT_TAG 23ba82c
    GIT_REPOSITORY https://github.com/eyalamirmusic/ResEmbed
)
unset(_resembed_patch)

# Embed font files into the app binary at build time.
# Sources compile once through UAPMD_COMPILE_TARGET; both executables share
# the resulting font-embedding object files.
res_embed_add(${UAPMD_COMPILE_TARGET}
    FILES
        "${UAPMD_EXTERNAL_DIR}/font/static/Roboto-SemiBold.ttf"
        "${fontaudio_SOURCE_DIR}/font/fontaudio.ttf"
        "${FontAwesome_SOURCE_DIR}/otfs/Font Awesome 7 Free-Solid-900.otf"
    NAMESPACE AppFonts
    CATEGORY AppFonts
)

# Embed built-in script presets into the app binary at build time.
# All .js files under scripts/ are treated as presets except the bridge
# library modules embedded separately below in AppJsLib.
file(GLOB UAPMD_APP_SCRIPT_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/*.js")
set(UAPMD_APP_JS_LIB_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/remidy-bridge.js"
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/uapmd-api.js"
)
list(REMOVE_ITEM UAPMD_APP_SCRIPT_FILES ${UAPMD_APP_JS_LIB_FILES})
list(SORT UAPMD_APP_SCRIPT_FILES)

# Each preset becomes accessible at runtime via
# ResEmbed::get("<filename>", "AppScripts").
res_embed_add(${UAPMD_COMPILE_TARGET}
    FILES
        ${UAPMD_APP_SCRIPT_FILES}
    NAMESPACE AppScripts
    CATEGORY AppScripts
)

# Embed the JS bridge library modules so the module resolver works on all
# platforms (Android, iOS, Emscripten) without requiring filesystem access.
# The desktop fallback (cpplocate-based filesystem search) is kept for
# development convenience when iterating on the JS sources without rebuilding.
res_embed_add(${UAPMD_COMPILE_TARGET}
    FILES
        ${UAPMD_APP_JS_LIB_FILES}
    NAMESPACE AppJsLib
    CATEGORY AppJsLib
)

if(NOT TARGET imgui)
    # Base ImGui sources
    set(IMGUI_SOURCES
            ${imgui_SOURCE_DIR}/imgui.cpp
            ${imgui_SOURCE_DIR}/imgui_demo.cpp
            ${imgui_SOURCE_DIR}/imgui_draw.cpp
            ${imgui_SOURCE_DIR}/imgui_tables.cpp
            ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    )

    # Add renderer-specific backend
    if(WIN32 AND USE_WIN32_DIRECTX11_BACKEND)
        list(APPEND IMGUI_SOURCES
            ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
            ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
        )
    elseif(IOS)
        # iOS: SDL_Renderer backend (SDL3 picks Metal automatically on iOS).
        # OpenGL ES is unavailable on iOS Simulator (Apple Silicon).
        list(APPEND IMGUI_SOURCES ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp)
    else()
        # OpenGL renderer for non-Win32, non-iOS backends
        list(APPEND IMGUI_SOURCES ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
    endif()

    # Add windowing backend-specific ImGui implementation (for SDL/GLFW)
    if(BACKEND_TYPE STREQUAL "SDL3")
        list(APPEND IMGUI_SOURCES ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp)
    elseif(BACKEND_TYPE STREQUAL "SDL2")
        list(APPEND IMGUI_SOURCES ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp)
    elseif(BACKEND_TYPE STREQUAL "GLFW")
        list(APPEND IMGUI_SOURCES ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp)
    endif()

    add_library(imgui STATIC ${IMGUI_SOURCES})

    target_include_directories(imgui PUBLIC
            ${imgui_SOURCE_DIR}
            ${imgui_SOURCE_DIR}/backends
    )
    # Link backend library to ImGui so backends can find headers
    target_link_libraries(imgui PUBLIC ${BACKEND_LIBRARIES})

    # Platform-specific rendering libraries
    if(WIN32 AND USE_WIN32_DIRECTX11_BACKEND)
        # Link DirectX 11 libraries
        target_link_libraries(imgui PUBLIC ${DX11_LIBRARIES})
        target_compile_definitions(imgui PUBLIC USE_DIRECTX11_RENDERER USE_WIN32_BACKEND)
    elseif(ANDROID)
        target_link_libraries(imgui PUBLIC GLESv3 EGL)
        target_compile_definitions(imgui PUBLIC IMGUI_IMPL_OPENGL_ES3)
    elseif(IOS)
        # SDL_Renderer on iOS maps to Metal; link Metal and MetalKit frameworks.
        target_link_libraries(imgui PUBLIC "-framework Metal" "-framework MetalKit")
        target_compile_definitions(imgui PUBLIC USE_SDL_RENDERER)
    elseif(APPLE)
        find_package(OpenGL REQUIRED)
        target_link_libraries(imgui PUBLIC OpenGL::GL)
    elseif(UNIX AND NOT EMSCRIPTEN)
        find_package(OpenGL REQUIRED)
        target_link_libraries(imgui PUBLIC OpenGL::GL)
    elseif(WIN32)
        # Windows OpenGL fallback
        target_link_libraries(imgui PUBLIC opengl32)
    endif()
endif()

if(NOT TARGET imnodes)
    add_library(imnodes STATIC
        ${imnodes_SOURCE_DIR}/imnodes.cpp
    )
    target_include_directories(imnodes PUBLIC
        ${imnodes_SOURCE_DIR}
    )
    target_link_libraries(imnodes PUBLIC imgui)
endif()

# Build ImTimeline as a static library with controlled public API
if(NOT TARGET imtimeline)
    set(IMTIMELINE_SOURCES
        ${ImTimeline_SOURCE_DIR}/ImTimeline.cpp
        ${ImTimeline_SOURCE_DIR}/Timeline.cpp
        ${ImTimeline_SOURCE_DIR}/TimelineCore/ImTimeline_internal.cpp
        ${ImTimeline_SOURCE_DIR}/TimelineCore/TimelinePlayer.cpp
        ${ImTimeline_SOURCE_DIR}/TimelineData/ImDataControllerVector.cpp
        ${ImTimeline_SOURCE_DIR}/TimelineViews/HorizontalNodeView.cpp
        ${ImTimeline_SOURCE_DIR}/TimelineViews/DebugPlayerView.cpp
    )

    add_library(imtimeline STATIC ${IMTIMELINE_SOURCES})

    # Public headers: ImTimeline.h, Timeline.h, and their required dependencies
    # TimelineCore and Core are needed by Timeline.h
    # TimelineViews and TimelineData are kept private (internal interfaces)
    target_include_directories(imtimeline
        PUBLIC
            ${ImTimeline_SOURCE_DIR}
            ${ImTimeline_SOURCE_DIR}/TimelineCore
            ${ImTimeline_SOURCE_DIR}/Core
        PRIVATE
            ${ImTimeline_SOURCE_DIR}/TimelineViews
            ${ImTimeline_SOURCE_DIR}/TimelineData
    )

    # ImTimeline depends on ImGui
    target_link_libraries(imtimeline PUBLIC imgui)
    target_compile_definitions(imtimeline PUBLIC IMGUI_DEFINE_MATH_OPERATORS)

    # Timeline.cpp calls ImGui::Text(variable) without a format string literal,
    # triggering -Wformat-security.  We suppress it here rather than patching the
    # third-party source.
    target_compile_options(imtimeline PRIVATE
        $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-Wno-format-security>
    )
endif()

# Add shared ImGui platform backend library
if(NOT TARGET remidy-imgui-shared)
    add_subdirectory(../remidy-imgui-shared ${CMAKE_CURRENT_BINARY_DIR}/remidy-imgui-shared)
endif()
target_link_libraries(remidy-imgui-shared PUBLIC imgui ${BACKEND_LIBRARIES})

# Add backend compile definition to the compile target (propagates to both
# executables via the OBJECT library link) and to remidy-imgui-shared.
if(WIN32 AND USE_WIN32_DIRECTX11_BACKEND)
    target_compile_definitions(remidy-imgui-shared PUBLIC USE_WIN32_BACKEND USE_DIRECTX11_RENDERER)
    target_compile_definitions(${UAPMD_COMPILE_TARGET} PRIVATE USE_WIN32_BACKEND USE_DIRECTX11_RENDERER)
elseif(BACKEND_TYPE STREQUAL "SDL3")
    target_compile_definitions(remidy-imgui-shared PUBLIC USE_SDL3_BACKEND)
    target_compile_definitions(${UAPMD_COMPILE_TARGET} PRIVATE USE_SDL3_BACKEND)
elseif(BACKEND_TYPE STREQUAL "SDL2")
    target_compile_definitions(remidy-imgui-shared PUBLIC USE_SDL2_BACKEND)
    target_compile_definitions(${UAPMD_COMPILE_TARGET} PRIVATE USE_SDL2_BACKEND)
elseif(BACKEND_TYPE STREQUAL "GLFW")
    target_compile_definitions(remidy-imgui-shared PUBLIC USE_GLFW_BACKEND)
    target_compile_definitions(${UAPMD_COMPILE_TARGET} PRIVATE USE_GLFW_BACKEND)
endif()

# UAPMD_HAS_MCP_SERVER     — MCP feature present (all platforms)
# UAPMD_MCP_HAS_HTTP_SERVER — embedded HTTP server mode available (desktop only)
target_compile_definitions(${UAPMD_COMPILE_TARGET} PRIVATE UAPMD_HAS_MCP_SERVER=1)
if(NOT UAPMD_BUILDING_WASM)
    target_link_libraries(${UAPMD_COMPILE_TARGET} PRIVATE ixwebsocket)
    if(NOT IOS AND NOT ANDROID)
        target_include_directories(${UAPMD_COMPILE_TARGET} PRIVATE ${cpp-httplib_SOURCE_DIR})
        target_compile_definitions(${UAPMD_COMPILE_TARGET} PRIVATE UAPMD_MCP_HAS_HTTP_SERVER=1)
    endif()
endif()

target_include_directories(${UAPMD_COMPILE_TARGET} PRIVATE
        ${choc_SOURCE_DIR}
        ${portable-file-dialogs_SOURCE_DIR}
        ${midicci_SOURCE_DIR}/include
        ${libremidi_SOURCE_DIR}/include
        ../../remidy/include
        ../../remidy-tooling/include
        ../../uapmd/include
        ../../uapmd-file/include
        ../../uapmd-data/include
        ../../uapmd-engine/include
        ../remidy-imgui-shared
        ${IconFontCppHeaders_SOURCE_DIR}
)
if(UAPMD_ENABLE_CPPLOCATE)
    target_include_directories(${UAPMD_COMPILE_TARGET} PRIVATE
            ${cpplocate_SOURCE_DIR}/source/cpplocate/include
            ${cpplocate_BINARY_DIR}/source/cpplocate/include
            ${cpplocate_BINARY_DIR}/source/liblocate/include
    )
endif()

# Enable ImGui vector operators for all app translation units
target_compile_definitions(remidy-imgui-shared PUBLIC IMGUI_DEFINE_MATH_OPERATORS)
target_compile_definitions(${UAPMD_COMPILE_TARGET} PRIVATE IMGUI_DEFINE_MATH_OPERATORS)

set(uapmd_LIBS_PLAT)
if(WIN32 AND NOT UAPMD_BUILDING_WASM)
    list(APPEND uapmd_LIBS_PLAT ws2_32)
endif()
if (${CMAKE_SYSTEM_NAME} STREQUAL "Darwin")
    set(uapmd_LIBS_PLAT
            "-framework AudioToolbox"
            "-framework AppKit"
            "-framework Cocoa"
    )
elseif(IOS)
    set(uapmd_LIBS_PLAT
            "-framework AudioToolbox"
            "-framework UIKit"
            "-framework Metal"
            "-framework MetalKit"
    )
endif ()

set(_uapmd_app_link_libraries
        imgui
        imnodes
        imtimeline
        ResEmbed
        remidy
        remidy-gui
        remidy-imgui-shared
        remidy-tooling
        uapmd
        uapmd-file
        midicci_static
        uapmd-engine
        libremidi::libremidi
        zlib-ng
)
if(UAPMD_ENABLE_CPPTRACE)
    list(APPEND _uapmd_app_link_libraries cpptrace::cpptrace)
endif()
if(NOT UAPMD_BUILDING_WASM)
    list(APPEND _uapmd_app_link_libraries ${uapmd_LIBS_PLAT})
endif()
if(UAPMD_ENABLE_CPPLOCATE)
    list(APPEND _uapmd_app_link_libraries ${cpplocate_LIBRARIES})
endif()

# Link the compile target (OBJECT library on desktop, direct executable on
# Android/WASM) against all app libraries.  PUBLIC scope causes both
# uapmd-app and uapmd-app-bundle to inherit every library transitively when
# they link against uapmd-app-objects below.
target_link_libraries(${UAPMD_COMPILE_TARGET} PUBLIC ${_uapmd_app_link_libraries})
if(WIN32 AND USE_WIN32_DIRECTX11_BACKEND)
    target_link_libraries(${UAPMD_COMPILE_TARGET} PUBLIC ${DX11_LIBRARIES})
endif()

# Wire entry-point targets to the compile target.
# Android: thin libmain.so links libuapmd-app.so (SDL3 JNI entry point).
# Desktop: thin executables link the OBJECT library.
if(ANDROID)
    target_link_libraries(main PRIVATE uapmd-app androidaudioplugin::androidaudioplugin)
elseif(IOS)
    # iOS: uapmd-app is built directly from UAPMD_COMPILE_TARGET (no OBJECT indirection)
elseif(NOT UAPMD_BUILDING_WASM)
    target_link_libraries(uapmd-app PRIVATE ${UAPMD_COMPILE_TARGET})
    if(APPLE)
        target_link_libraries(uapmd-app-bundle PRIVATE ${UAPMD_COMPILE_TARGET})
    endif()
endif()

# Create a .dSYM file on macOS for each executable (Xcode handles this for iOS)
if(APPLE AND NOT UAPMD_BUILDING_WASM AND NOT IOS)
    add_custom_command(TARGET uapmd-app POST_BUILD
            COMMAND dsymutil $<TARGET_FILE:uapmd-app>)
    add_custom_command(TARGET uapmd-app-bundle POST_BUILD
            COMMAND dsymutil $<TARGET_FILE:uapmd-app-bundle>)
endif()

target_compile_features(${UAPMD_COMPILE_TARGET} PRIVATE cxx_std_23)

if(IOS)
    set(UAPMD_APP_IOS_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/UapmdApp-iOS-Info.plist")
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/Info-iOS.plist.in ${UAPMD_APP_IOS_INFO_PLIST} @ONLY)
    set_target_properties(uapmd-app PROPERTIES
            MACOSX_BUNDLE_INFO_PLIST ${UAPMD_APP_IOS_INFO_PLIST}
            XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
            XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "NO"
            XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY ""
    )
elseif(APPLE AND NOT UAPMD_BUILDING_WASM)
    set(UAPMD_APP_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/UapmdApp-Info.plist")
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in ${UAPMD_APP_INFO_PLIST} @ONLY)
    set_target_properties(uapmd-app-bundle PROPERTIES
            MACOSX_BUNDLE_INFO_PLIST ${UAPMD_APP_INFO_PLIST}
            OUTPUT_NAME "uapmd-app"
    )

    add_custom_command(TARGET uapmd-app-bundle POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_BUNDLE_CONTENT_DIR:uapmd-app-bundle>/Resources"
        COMMAND ${CMAKE_COMMAND} -E copy
            ${UAPMD_RESOURCES_DIR}/uapmd.icns
            "$<TARGET_BUNDLE_CONTENT_DIR:uapmd-app-bundle>/Resources/uapmd.icns"
        COMMENT "Copying resources into the macOS bundle"
    )
endif()


# Android-specific configuration (targets uapmd-app, not main)
if(ANDROID)
    target_link_libraries(uapmd-app PUBLIC
        android  # Android native APIs
        log      # Android logging
    )
    target_compile_definitions(uapmd-app PRIVATE
        IMGUI_IMPL_OPENGL_ES3  # Use OpenGL ES 3.0 instead of desktop GL
        __ANDROID__=1          # Android platform marker
    )
    find_package(SDL3 REQUIRED CONFIG)
    target_link_libraries(uapmd-app PUBLIC SDL3::SDL3)
    message(STATUS "Android build configured with SDL3 support")
endif()

# iOS-specific configuration
if(IOS)
    target_compile_definitions(uapmd-app PRIVATE
        USE_SDL_RENDERER       # Use SDL_Renderer (Metal-backed) instead of OpenGL
    )
    # BACKEND_LIBRARIES was resolved above (find_package or CPM); use it directly.
    target_link_libraries(uapmd-app PUBLIC ${BACKEND_LIBRARIES})
    message(STATUS "iOS build configured with SDL3 + SDL_Renderer (Metal) support")
endif()
