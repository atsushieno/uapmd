# Platform-specific container window implementations
if(ANDROID)
    add_library(remidy-gui STATIC
        ContainerWindow_Android.cpp
    )
    target_link_libraries(remidy-gui PUBLIC
            GLESv3
    )
elseif(APPLE)
    add_library(remidy-gui STATIC
        ContainerWindow_Cocoa.mm
    )
    target_compile_options(remidy-gui PRIVATE
        -fobjc-arc
    )
    target_link_libraries(remidy-gui PUBLIC
        "-framework Cocoa"
    )
elseif(WIN32)
    add_library(remidy-gui STATIC
        ContainerWindow_Win32.cpp
    )
    # Win32 libraries linked automatically via pragma comment
elseif(EMSCRIPTEN)
    add_library(remidy-gui STATIC
        ContainerWindow_Emscripten.cpp
    )
elseif(UNIX)
    add_library(remidy-gui STATIC
        ContainerWindow_X11.cpp
    )
    if (NOT EMSCRIPTEN)
        find_package(X11 REQUIRED)
        target_link_libraries(remidy-gui PUBLIC
            X11::X11
        )
    endif()
endif()

# Include directories
target_include_directories(remidy-gui PUBLIC
    ../../include
)

# Set C++ standard
set_target_properties(remidy-gui PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

add_library(remidy::remidy-gui ALIAS remidy-gui)
