add_executable(uapmd-service)

option(UAPMD_SERVICE_ENABLE_GUI "Build the graphical frontend for uapmd-service" ON)

set(UAPMD_SERVICE_GUI_ENABLED FALSE)
set(UAPMD_GUI_BACKEND_FOUND FALSE)
set(UAPMD_GUI_BACKEND_TYPE "")
set(UAPMD_GUI_BACKEND_LIBRARIES "")

if(UAPMD_SERVICE_ENABLE_GUI)
    # Try SDL3 first
    find_package(SDL3 QUIET)
    if(SDL3_FOUND)
        set(UAPMD_GUI_BACKEND_FOUND TRUE)
        set(UAPMD_GUI_BACKEND_TYPE "SDL3")
        list(APPEND UAPMD_GUI_BACKEND_LIBRARIES SDL3::SDL3)
    endif()

    # Try SDL2 if SDL3 not found
    if(NOT UAPMD_GUI_BACKEND_FOUND)
        find_package(SDL2 QUIET)
        if(SDL2_FOUND)
            set(UAPMD_GUI_BACKEND_FOUND TRUE)
            set(UAPMD_GUI_BACKEND_TYPE "SDL2")
            list(APPEND UAPMD_GUI_BACKEND_LIBRARIES SDL2::SDL2)
        endif()
    endif()

    # Fallback to GLFW if neither SDL backend found
    if(NOT UAPMD_GUI_BACKEND_FOUND)
        find_package(glfw3 QUIET)
        if(glfw3_FOUND)
            set(UAPMD_GUI_BACKEND_FOUND TRUE)
            set(UAPMD_GUI_BACKEND_TYPE "GLFW")
            list(APPEND UAPMD_GUI_BACKEND_LIBRARIES glfw)
        else()
            message(STATUS "No system GLFW found for uapmd-service GUI, adding via CPM")
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
                set(UAPMD_GUI_BACKEND_FOUND TRUE)
                set(UAPMD_GUI_BACKEND_TYPE "GLFW")
                list(APPEND UAPMD_GUI_BACKEND_LIBRARIES glfw)
            endif()
        endif()
    endif()

    if(UAPMD_GUI_BACKEND_FOUND)
        set(UAPMD_SERVICE_GUI_ENABLED TRUE)
    else()
        message(WARNING "No windowing backend found for uapmd-service GUI; building headless variant")
    endif()
endif()

if(UAPMD_SERVICE_GUI_ENABLED)
    if(NOT DEFINED imgui_SOURCE_DIR)
        CPMAddPackage(
                NAME imgui
                GIT_TAG v1.92.3
                GIT_REPOSITORY https://github.com/ocornut/imgui
                DOWNLOAD_ONLY YES
        )
    endif()

    if(NOT TARGET imgui_uapmd)
        set(IMGUI_UAPMD_SOURCES
                ${imgui_SOURCE_DIR}/imgui.cpp
                ${imgui_SOURCE_DIR}/imgui_demo.cpp
                ${imgui_SOURCE_DIR}/imgui_draw.cpp
                ${imgui_SOURCE_DIR}/imgui_tables.cpp
                ${imgui_SOURCE_DIR}/imgui_widgets.cpp
                ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
        )
        if(UAPMD_GUI_BACKEND_TYPE STREQUAL "SDL3")
            list(APPEND IMGUI_UAPMD_SOURCES ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp)
        elseif(UAPMD_GUI_BACKEND_TYPE STREQUAL "SDL2")
            list(APPEND IMGUI_UAPMD_SOURCES ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp)
        elseif(UAPMD_GUI_BACKEND_TYPE STREQUAL "GLFW")
            list(APPEND IMGUI_UAPMD_SOURCES ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp)
        endif()

        add_library(imgui_uapmd STATIC ${IMGUI_UAPMD_SOURCES})
        target_include_directories(imgui_uapmd PUBLIC
                ${imgui_SOURCE_DIR}
                ${imgui_SOURCE_DIR}/backends
        )
        target_link_libraries(imgui_uapmd PUBLIC ${UAPMD_GUI_BACKEND_LIBRARIES})

        if(APPLE)
            find_package(OpenGL REQUIRED)
            target_link_libraries(imgui_uapmd PUBLIC OpenGL::GL)
        elseif(UNIX)
            find_package(OpenGL REQUIRED)
            target_link_libraries(imgui_uapmd PUBLIC OpenGL::GL)
        elseif(WIN32)
            target_link_libraries(imgui_uapmd PUBLIC opengl32)
        endif()
    endif()
endif()

target_include_directories(uapmd-service PRIVATE
        ../../include
        ../../src/remidy-tooling
        ../../external/choc
        ../../external/libremidi/include
        ../../external/cpplocate/source/cpplocate/include
        ${midicci_SOURCE_DIR}/include
        ${cmidi2_SOURCE_DIR}
)

if (${CMAKE_SYSTEM_NAME} STREQUAL "Darwin")
    set(uapmd-service_LIBS_PLAT
            "-framework AudioToolbox"
            "-framework AppKit"
    )
endif ()

set(UAPMD_SERVICE_SOURCES
        main.cpp
        Configuration/AudioDeviceConfiguration.cpp
        Configuration/VirtualMidiDeviceConfiguration.cpp
        Controller/VirtualMidiDeviceController.cpp
        Shell/CommandShell.cpp
        VirtualMidiDevices/impl/PlatformVirtualMidiDeviceImpl.cpp
        VirtualMidiDevices/PlatformVirtualMidiDevice.cpp
        VirtualMidiDevices/UapmdMidiDevice.cpp
)

target_sources(uapmd-service PRIVATE ${UAPMD_SERVICE_SOURCES})

if(UAPMD_SERVICE_GUI_ENABLED)
    target_sources(uapmd-service PRIVATE
            Gui/GuiApp.cpp
            Gui/MainWindow.cpp
            Gui/PlatformBackend.cpp
    )
endif()

set(UAPMD_SERVICE_LIBRARIES
        uapmd
        cpplocate::cpplocate
        cpptrace::cpptrace
        libremidi::libremidi
        midicci
        ${uapmd-service_LIBS_PLAT}
)

if(UAPMD_SERVICE_GUI_ENABLED)
    list(APPEND UAPMD_SERVICE_LIBRARIES imgui_uapmd ${UAPMD_GUI_BACKEND_LIBRARIES} remidy-gui)
endif()

target_link_libraries(uapmd-service PRIVATE ${UAPMD_SERVICE_LIBRARIES})

# On Linux, link X11 for GLX-based context guarding
if(UNIX AND NOT APPLE)
    find_package(X11 REQUIRED)
    target_link_libraries(uapmd-service PRIVATE X11::X11)
endif()

if(UAPMD_SERVICE_GUI_ENABLED)
    target_compile_definitions(uapmd-service PRIVATE UAPMD_SERVICE_HAS_GUI)
    if(UAPMD_GUI_BACKEND_TYPE STREQUAL "SDL3")
        target_compile_definitions(uapmd-service PRIVATE USE_SDL3_BACKEND)
    elseif(UAPMD_GUI_BACKEND_TYPE STREQUAL "SDL2")
        target_compile_definitions(uapmd-service PRIVATE USE_SDL2_BACKEND)
    elseif(UAPMD_GUI_BACKEND_TYPE STREQUAL "GLFW")
        target_compile_definitions(uapmd-service PRIVATE USE_GLFW_BACKEND)
    endif()
endif()
