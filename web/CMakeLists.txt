cmake_minimum_required(VERSION 3.21)

project(uapmd-wasm VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT EMSCRIPTEN)
    message(FATAL_ERROR "This CMakeLists.txt is for Emscripten/WebAssembly builds only")
endif()

message(STATUS "Building for Emscripten/Wasm")

# Define for WebAssembly build
add_compile_definitions(EMSCRIPTEN=1)
add_compile_definitions(USE_GLFW_BACKEND=1)
add_compile_definitions(UAPMD_WEB_BUILD=1)
# Ensure ImGui OpenGL3 backend uses GLES3 path on web
add_compile_definitions(IMGUI_IMPL_OPENGL_ES3=1)

# Disable all platform-specific features for WebAssembly
add_compile_definitions(DISABLE_VST3=1)
add_compile_definitions(DISABLE_AU=1)
add_compile_definitions(DISABLE_LV2=1)
add_compile_definitions(DISABLE_CLAP=1)
add_compile_definitions(DISABLE_PLATFORM_MIDI=1)  # No Web MIDI 2.0 API
add_compile_definitions(DISABLE_X11=1)
add_compile_definitions(DISABLE_GTK=1)
add_compile_definitions(DISABLE_NATIVE_AUDIO=1)  # Will use Web Audio API later

# Disable package finding for desktop-only dependencies
set(CMAKE_DISABLE_FIND_PACKAGE_X11 ON)
set(CMAKE_DISABLE_FIND_PACKAGE_GTK ON)
set(CMAKE_DISABLE_FIND_PACKAGE_PkgConfig ON)

# Emscripten-specific compiler flags
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -std=c++23 -s DISABLE_EXCEPTION_CATCHING=1 --use-port=contrib.glfw3")

# Emscripten-specific linker flags
set(CMAKE_EXE_LINKER_FLAGS
    "${CMAKE_EXE_LINKER_FLAGS} \
    -s WASM=1 \
    -s ALLOW_MEMORY_GROWTH=1 \
    -s MAXIMUM_MEMORY=512MB \
    -s USE_WEBGL2=1 \
    -s FULL_ES3=1 \
    -s MIN_WEBGL_VERSION=2 \
    -s MAX_WEBGL_VERSION=2 \
    -s DISABLE_EXCEPTION_CATCHING=1 \
    --use-port=contrib.glfw3 \
    -s NO_EXIT_RUNTIME=0 \
    -s ASSERTIONS=1 \
    -s EXPORTED_RUNTIME_METHODS=['ccall','cwrap','getValue','setValue','UTF8ToString','stringToUTF8']")

# Include CPM
set(CPM_SOURCE_CACHE "${CMAKE_BINARY_DIR}/_deps")
get_filename_component(UAPMD_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
include("${UAPMD_ROOT}/cmake/CPM.cmake")

# Skip these dependencies for minimal build
# TODO: Add back when needed
# CPMAddPackage(
#         NAME choc
#         GIT_TAG         68a63dd
#         GIT_REPOSITORY  https://github.com/Tracktion/choc
#         DOWNLOAD_ONLY YES
# )
#
# CPMAddPackage(
#         NAME portable-file-dialogs
#         GIT_TAG c12ea8c
#         GIT_REPOSITORY https://github.com/samhocevar/portable-file-dialogs
#         DOWNLOAD_ONLY YES
# )
#
# CPMAddPackage(
#         NAME concurrentqueue
#         GIT_TAG        2f09da73d22a47dc8a89cdd4fc4c3bfae07f4284
#         GIT_REPOSITORY https://github.com/cameron314/concurrentqueue.git
#         DOWNLOAD_ONLY YES
# )

# Skip these dependencies for minimal build - not needed for basic ImGui demo
# TODO: Add back when we need MIDI/plugin functionality
# set(CMAKE_SKIP_INSTALL_RULES ON)
# FetchContent_Declare(cmidi2
#         GIT_REPOSITORY https://github.com/atsushieno/cmidi2
#         GIT_TAG        0.9.3
#         EXCLUDE_FROM_ALL
# )
# FetchContent_MakeAvailable(cmidi2)
#
# FetchContent_Declare(cpplocate
#         NAME           cpplocate
#         GIT_TAG        b796990d8df747758dbd6217b6315b7bdbb0c27e
#         GIT_REPOSITORY https://github.com/cginternals/cpplocate
#         EXCLUDE_FROM_ALL
# )
# set(_UAPMD_OLD_BUILD_SHARED_LIBS_CPPL "${BUILD_SHARED_LIBS}")
# set(BUILD_SHARED_LIBS OFF)
# set(OPTION_BUILD_TESTS OFF CACHE BOOL "Disable cpplocate tests for web" FORCE)
# set(OPTION_BUILD_DOCS OFF CACHE BOOL "Disable cpplocate docs for web" FORCE)
# FetchContent_MakeAvailable(cpplocate)
# if(DEFINED _UAPMD_OLD_BUILD_SHARED_LIBS_CPPL AND NOT "${_UAPMD_OLD_BUILD_SHARED_LIBS_CPPL}" STREQUAL "")
#     set(BUILD_SHARED_LIBS "${_UAPMD_OLD_BUILD_SHARED_LIBS_CPPL}")
# else()
#     unset(_UAPMD_OLD_BUILD_SHARED_LIBS_CPPL)
# endif()
#
# FetchContent_Declare(midicci
#         NAME            midicci
#         GIT_TAG         8d68cb2
#         GIT_REPOSITORY https://github.com/atsushieno/midicci
#         EXCLUDE_FROM_ALL
# )
# set(_UAPMD_OLD_BUILD_SHARED_LIBS "${BUILD_SHARED_LIBS}")
# set(BUILD_SHARED_LIBS OFF)
# # Disable midicci tools and GUI components for WebAssembly
# set(MIDICCI_BUILD_TOOLS OFF CACHE BOOL "Disable midicci tools for web" FORCE)
# set(MIDICCI_BUILD_TESTS OFF CACHE BOOL "Disable midicci tests for web" FORCE)
# FetchContent_MakeAvailable(midicci)
# if(DEFINED _UAPMD_OLD_BUILD_SHARED_LIBS AND NOT "${_UAPMD_OLD_BUILD_SHARED_LIBS}" STREQUAL "")
#     set(BUILD_SHARED_LIBS "${_UAPMD_OLD_BUILD_SHARED_LIBS}")
# else()
#     unset(_UAPMD_OLD_BUILD_SHARED_LIBS)
# endif()
# set(CMAKE_SKIP_INSTALL_RULES OFF)

# Add ImGui via FetchContent
CPMAddPackage(
        NAME imgui
        GIT_TAG        v1.92.3
        GIT_REPOSITORY https://github.com/ocornut/imgui
        DOWNLOAD_ONLY YES
)

# Skip remidy library for now - it has VST3/CLAP/LV2 dependencies that don't build with Emscripten
# TODO: Create web-specific stub implementation later
# set(CMAKE_SKIP_INSTALL_RULES ON)
# add_subdirectory(${UAPMD_ROOT}/src/remidy ${CMAKE_BINARY_DIR}/remidy)
# set(CMAKE_SKIP_INSTALL_RULES OFF)

# Build WebAssembly uapmd-app with GUI + stubs
add_executable(uapmd-app
    # Web-specific main
    web_main.cpp

    # Real GUI sources (desktop code reused)
    ${UAPMD_ROOT}/tools/uapmd-app/gui/AudioDeviceSettings.cpp
    ${UAPMD_ROOT}/tools/uapmd-app/gui/PluginList.cpp
    ${UAPMD_ROOT}/tools/uapmd-app/gui/PluginSelector.cpp
    ${UAPMD_ROOT}/tools/uapmd-app/gui/SpectrumAnalyzer.cpp
    ${UAPMD_ROOT}/tools/uapmd-app/gui/TrackList.cpp
    ${UAPMD_ROOT}/tools/uapmd-app/gui/MainWindow.cpp

    # Web stub implementations
    stubs/remidy_gui_stubs.cpp
    stubs/remidy_event_loop_stub.cpp
    stubs/remidy_logger_stub.cpp
    stubs/audio_stubs.cpp
    stubs/midi_stubs.cpp
    stubs/sequencer_stubs.cpp

    # ImGui sources
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)

# Include directories
target_include_directories(uapmd-app PRIVATE
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    # Prefer web stubs first to override desktop headers
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${UAPMD_ROOT}/tools/remidy-imgui-shared
    ${CMAKE_CURRENT_SOURCE_DIR}
    # Project headers used by GUI sources
    ${UAPMD_ROOT}/include
    ${UAPMD_ROOT}/tools
)

# Find and link required libraries
find_package(Threads REQUIRED)

# Link libraries for Emscripten
target_link_libraries(uapmd-app PRIVATE
    Threads::Threads
)
