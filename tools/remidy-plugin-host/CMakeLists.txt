cmake_minimum_required(VERSION 3.18)
if (APPLE)
    project(remidy LANGUAGES C CXX OBJC OBJCXX Swift)
else()
    project(remidy LANGUAGES C CXX)
endif()
set(CMAKE_CXX_STANDARD 23)

add_executable(remidy-plugin-host-imgui)

# GLFW for windowing (must come before ImGui)
CPMAddPackage(
        NAME glfw
        GIT_TAG 3.4
        GIT_REPOSITORY https://github.com/glfw/glfw
        OPTIONS
        "GLFW_BUILD_DOCS OFF"
        "GLFW_BUILD_TESTS OFF"
        "GLFW_BUILD_EXAMPLES OFF"
        "GLFW_INSTALL OFF"
)

# ImGui and backend dependencies
CPMAddPackage(
        NAME imgui
        GIT_TAG v1.91.0
        GIT_REPOSITORY https://github.com/ocornut/imgui
        DOWNLOAD_ONLY YES
)

if(imgui_ADDED)
    add_library(imgui STATIC
            ${imgui_SOURCE_DIR}/imgui.cpp
            ${imgui_SOURCE_DIR}/imgui_demo.cpp
            ${imgui_SOURCE_DIR}/imgui_draw.cpp
            ${imgui_SOURCE_DIR}/imgui_tables.cpp
            ${imgui_SOURCE_DIR}/imgui_widgets.cpp
            ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
            ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    )

    target_include_directories(imgui PUBLIC
            ${imgui_SOURCE_DIR}
            ${imgui_SOURCE_DIR}/backends
    )

    # Link GLFW to ImGui so backends can find headers
    target_link_libraries(imgui PUBLIC glfw)

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

target_include_directories(remidy-plugin-host-imgui PRIVATE
        ../../external/choc
        ${cmidi2_SOURCE_DIR}
        ../../include
)

target_link_libraries(remidy-plugin-host-imgui PRIVATE
        imgui
        remidy::remidy
        remidy::remidy-tooling
        cpptrace::cpptrace
        uapmd
        midicci
)

target_sources(remidy-plugin-host-imgui PRIVATE
        main.cpp
        AppModel.cpp
        gui/MainWindow.cpp
        gui/MidiKeyboard.cpp
)

# Use C++23 like the WebView version
target_compile_features(remidy-plugin-host-imgui PRIVATE cxx_std_23)

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