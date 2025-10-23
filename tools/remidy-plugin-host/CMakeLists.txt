cmake_minimum_required(VERSION 3.18)
if (APPLE)
    project(remidy LANGUAGES C CXX OBJC OBJCXX)
else()
    project(remidy LANGUAGES C CXX)
endif()
set(CMAKE_CXX_STANDARD 23)

add_executable(remidy-plugin-host)

# Backend selection with priority: SDL3 > SDL2 > GLFW
set(BACKEND_FOUND FALSE)
set(BACKEND_TYPE "")
set(BACKEND_LIBRARIES "")
set(BACKEND_INCLUDE_DIRS "")

# Try SDL3 first
find_package(SDL3 QUIET)
if(SDL3_FOUND)
    set(BACKEND_FOUND TRUE)
    set(BACKEND_TYPE "SDL3")
    set(BACKEND_LIBRARIES SDL3::SDL3)
    message(STATUS "Using SDL3 backend")
    add_compile_definitions(USE_SDL3_BACKEND)
endif()

## Native embedding via X11 on Linux (auto, no option)

# Try SDL2 if SDL3 not found
if(NOT BACKEND_FOUND)
    find_package(SDL2 QUIET)
    if(SDL2_FOUND)
        set(BACKEND_FOUND TRUE)
        set(BACKEND_TYPE "SDL2")
        set(BACKEND_LIBRARIES SDL2::SDL2)
        message(STATUS "Using SDL2 backend")
        add_compile_definitions(USE_SDL2_BACKEND)
    endif()
endif()

# Fallback to GLFW if neither SDL3 nor SDL2 found
if(NOT BACKEND_FOUND)
    find_package(glfw3 QUIET)
    if(glfw3_FOUND)
        set(BACKEND_FOUND TRUE)
        set(BACKEND_TYPE "GLFW")
        set(BACKEND_LIBRARIES glfw)
        message(STATUS "Using GLFW backend")
        add_compile_definitions(USE_GLFW_BACKEND)
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
            message(STATUS "Using GLFW backend (via CPM)")
            add_compile_definitions(USE_GLFW_BACKEND)
        else()
            message(FATAL_ERROR "Failed to add GLFW via CPM")
        endif()
    endif()
endif()

# ImGui and backend dependencies
CPMAddPackage(
        NAME imgui
        GIT_TAG v1.92.3
        GIT_REPOSITORY https://github.com/ocornut/imgui
        DOWNLOAD_ONLY YES
)

if(imgui_ADDED)
    # Base ImGui sources
    set(IMGUI_SOURCES
            ${imgui_SOURCE_DIR}/imgui.cpp
            ${imgui_SOURCE_DIR}/imgui_demo.cpp
            ${imgui_SOURCE_DIR}/imgui_draw.cpp
            ${imgui_SOURCE_DIR}/imgui_tables.cpp
            ${imgui_SOURCE_DIR}/imgui_widgets.cpp
            ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    )

    # Add backend-specific ImGui implementation
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

    # Platform-specific OpenGL and windowing
    if(APPLE)
        find_package(OpenGL REQUIRED)
        target_link_libraries(imgui PUBLIC OpenGL::GL)
    elseif(UNIX)
        find_package(OpenGL REQUIRED)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(GL REQUIRED gl)
        target_link_libraries(imgui PUBLIC OpenGL::GL ${GL_LIBRARIES})
    elseif(WIN32)
        target_link_libraries(imgui PUBLIC opengl32)
    endif()
endif()

target_include_directories(remidy-plugin-host PRIVATE
        ../../external/choc
        ${cmidi2_SOURCE_DIR}
        ../../include
)

target_link_libraries(remidy-plugin-host PRIVATE
        imgui
        cpptrace::cpptrace
        uapmd
        remidy-gui
)

# On Linux, CHOC's DesktopWindow uses GTK3; ensure headers and libs are present
if(UNIX AND NOT APPLE)
    find_package(X11 REQUIRED)
    target_link_libraries(remidy-plugin-host PRIVATE X11::X11)
endif()
if(APPLE)
    target_link_libraries(remidy-plugin-host PRIVATE "-framework Cocoa")
endif()

target_sources(remidy-plugin-host PRIVATE
        main.cpp
        AppModel.cpp
        gui/MainWindow.cpp
        gui/MidiKeyboard.cpp
        gui/PlatformBackend.cpp
)

# Use C++23 like the WebView version
target_compile_features(remidy-plugin-host PRIVATE cxx_std_23)

# Copy over shared AppModel from WebView version for now
configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/../remidy-plugin-host/AppModel.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/AppModel.hpp
        COPYONLY
)
configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/../remidy-plugin-host/AppModel.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/AppModel.cpp
        COPYONLY
)
