cmake_minimum_required(VERSION 3.18)

# Shared ImGui platform backend library for remidy tools
# This library provides SDL3/SDL2/GLFW backend abstractions with ImGui support
# Used by both remidy-plugin-host and uapmd-service

add_library(remidy-imgui-shared STATIC
    PlatformBackend.cpp
)

target_include_directories(remidy-imgui-shared PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ../../external/choc
)

# This library requires imgui and a windowing backend (SDL3/SDL2/GLFW)
# The parent project must:
# - Define USE_SDL3_BACKEND, USE_SDL2_BACKEND, or USE_GLFW_BACKEND
# - Link imgui and the appropriate backend library to this target

set_target_properties(remidy-imgui-shared PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

add_library(remidy::remidy-imgui-shared ALIAS remidy-imgui-shared)
