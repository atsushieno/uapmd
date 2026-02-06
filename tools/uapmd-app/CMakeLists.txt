cmake_minimum_required(VERSION 3.18)
if (APPLE)
    project(uapmd-app LANGUAGES C CXX OBJC OBJCXX)
else()
    project(uapmd-app LANGUAGES C CXX)
endif()
set(CMAKE_CXX_STANDARD 23)

add_executable(uapmd-app)
set(UAPMD_APP_TARGETS uapmd-app)

if(APPLE)
    add_executable(uapmd-app-bundle MACOSX_BUNDLE)
    list(APPEND UAPMD_APP_TARGETS uapmd-app-bundle)
endif()

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
    add_compile_definitions(USE_SDL3_BACKEND)
    message(STATUS "Using SDL3 backend")
endif()

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

# ImTimeline for SequenceEditor timeline view
CPMAddPackage(
    NAME ImTimeline
    GIT_TAG fixes
    GIT_REPOSITORY https://github.com/triplejam/ImTimeline
    DOWNLOAD_ONLY YES
)

if(NOT TARGET imgui)
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
        target_link_libraries(imgui PUBLIC OpenGL::GL)
    elseif(WIN32)
        target_link_libraries(imgui PUBLIC opengl32)
    endif()
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
endif()

# Add shared ImGui platform backend library
if(NOT TARGET remidy-imgui-shared)
    add_subdirectory(../remidy-imgui-shared ${CMAKE_CURRENT_BINARY_DIR}/remidy-imgui-shared)
endif()
target_link_libraries(remidy-imgui-shared PUBLIC imgui ${BACKEND_LIBRARIES})

# Add backend compile definition to both targets
if(BACKEND_TYPE STREQUAL "SDL3")
    target_compile_definitions(remidy-imgui-shared PUBLIC USE_SDL3_BACKEND)
    foreach(app_target IN LISTS UAPMD_APP_TARGETS)
        target_compile_definitions(${app_target} PRIVATE USE_SDL3_BACKEND)
    endforeach()
elseif(BACKEND_TYPE STREQUAL "SDL2")
    target_compile_definitions(remidy-imgui-shared PUBLIC USE_SDL2_BACKEND)
    foreach(app_target IN LISTS UAPMD_APP_TARGETS)
        target_compile_definitions(${app_target} PRIVATE USE_SDL2_BACKEND)
    endforeach()
elseif(BACKEND_TYPE STREQUAL "GLFW")
    target_compile_definitions(remidy-imgui-shared PUBLIC USE_GLFW_BACKEND)
    foreach(app_target IN LISTS UAPMD_APP_TARGETS)
        target_compile_definitions(${app_target} PRIVATE USE_GLFW_BACKEND)
    endforeach()
endif()

foreach(app_target IN LISTS UAPMD_APP_TARGETS)
    target_include_directories(${app_target} PRIVATE
            ${choc_SOURCE_DIR}
            ${portable-file-dialogs_SOURCE_DIR}
            ${midicci_SOURCE_DIR}/include
            ../../include
            ${libremidi_SOURCE_DIR}/include
            ${cpplocate_SOURCE_DIR}/include
            ../remidy-imgui-shared
    )
endforeach()

# Enable ImGui vector operators for all app translation units
target_compile_definitions(remidy-imgui-shared PUBLIC IMGUI_DEFINE_MATH_OPERATORS)
foreach(app_target IN LISTS UAPMD_APP_TARGETS)
    target_compile_definitions(${app_target} PRIVATE IMGUI_DEFINE_MATH_OPERATORS)
endforeach()

set(uapmd_LIBS_PLAT)
if (${CMAKE_SYSTEM_NAME} STREQUAL "Darwin")
    set(uapmd_LIBS_PLAT
            "-framework AudioToolbox"
            "-framework AppKit"
            "-framework Cocoa"
    )
endif ()

foreach(app_target IN LISTS UAPMD_APP_TARGETS)
    target_link_libraries(${app_target} PRIVATE
            imgui
            imtimeline
            cpptrace::cpptrace
            uapmd
            uapmd-engine
            remidy-gui
            remidy-imgui-shared
            cpplocate::cpplocate
            libremidi::libremidi
            midicci_static
            zlib-ng
            ${uapmd_LIBS_PLAT}
    )

    # Create a .dSYM file on macOS
    if(APPLE)
        add_custom_command(
                TARGET ${app_target}
                POST_BUILD
                COMMAND dsymutil $<TARGET_FILE:${app_target}>
        )
    endif()
endforeach()

# On Linux, CHOC's DesktopWindow uses GTK3; ensure headers and libs are present
if(UNIX AND NOT APPLE)
    find_package(X11 REQUIRED)
    target_link_libraries(uapmd-app PRIVATE X11::X11)
endif()

set(UAPMD_APP_SOURCE_FILES
        main.cpp
        AppModel.cpp
        UapmdJSRuntime.cpp
        gui/MainWindow.cpp
        gui/PluginList.cpp
        gui/InstanceDetails.cpp
        gui/PluginSelector.cpp
        gui/ParameterList.cpp
        gui/TrackList.cpp
        gui/AudioDeviceSettings.cpp
        gui/ScriptEditor.cpp
        gui/SpectrumAnalyzer.cpp
        gui/SequenceEditor.cpp
        gui/FontLoader.cpp
        Configuration/AudioDeviceConfiguration.cpp
        Configuration/VirtualMidiDeviceConfiguration.cpp
)

foreach(app_target IN LISTS UAPMD_APP_TARGETS)
    target_sources(${app_target} PRIVATE ${UAPMD_APP_SOURCE_FILES})
endforeach()

foreach(app_target IN LISTS UAPMD_APP_TARGETS)
    target_compile_features(${app_target} PRIVATE cxx_std_23)
endforeach()

if(APPLE)
    set(UAPMD_APP_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/UapmdApp-Info.plist")
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in ${UAPMD_APP_INFO_PLIST} @ONLY)
    set_target_properties(uapmd-app-bundle PROPERTIES
            MACOSX_BUNDLE_INFO_PLIST ${UAPMD_APP_INFO_PLIST}
            OUTPUT_NAME "uapmd-app"
    )

    add_custom_command(TARGET uapmd-app-bundle POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_BUNDLE_CONTENT_DIR:uapmd-app-bundle>/Resources"
        COMMAND ${CMAKE_COMMAND} -E remove_directory
            "$<TARGET_BUNDLE_CONTENT_DIR:uapmd-app-bundle>/Resources/js"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/js
            "$<TARGET_BUNDLE_CONTENT_DIR:uapmd-app-bundle>/Resources/js"
        COMMENT "Copying js resources into the macOS bundle"
    )
endif()

# Create symbolic link to js directory for script editor module resolution
add_custom_command(TARGET uapmd-app POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E create_symlink
        ${CMAKE_SOURCE_DIR}/js
        $<TARGET_FILE_DIR:uapmd-app>/js
    COMMENT "Creating symbolic link to js directory for module resolution"
)
