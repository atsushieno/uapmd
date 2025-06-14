add_executable(remidy-plugin-host)

set(saucer_modules ON)

if(UNIX AND NOT APPLE)
    set(saucer_backend "Qt5")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GTK3 REQUIRED gtk+-3.0)
    pkg_check_modules(WEBKITGTK REQUIRED webkit2gtk-4.1)
endif()

if (WIN32)
    cmake_path(SET SAUCER_PATCH_FILE NORMALIZE "${CMAKE_CURRENT_SOURCE_DIR}/../../external/saucer-disable-polyfill-msvc.patch")
elseif (APPLE)
    cmake_path(SET SAUCER_PATCH_FILE NORMALIZE "${CMAKE_CURRENT_SOURCE_DIR}/../../external/saucer-disable-polyfill-apple.patch")
else ()
    cmake_path(SET SAUCER_PATCH_FILE NORMALIZE "${CMAKE_CURRENT_SOURCE_DIR}/../../external/saucer-disable-polyfill-linux.patch")
endif()
FetchContent_Declare(saucer
        GIT_TAG        41e9ce4914261e8f71cfab960d38d6bc76a0e2ec
        GIT_REPOSITORY "https://github.com/saucer/saucer"
        EXCLUDE_FROM_ALL
        #PATCH_COMMAND  git apply "${SAUCER_PATCH_FILE}"
        #UPDATE_DISCONNECTED 1 # https://stackoverflow.com/questions/73718516/
)
FetchContent_MakeAvailable(saucer)

target_compile_options(remidy-plugin-host PRIVATE
        ${GTK3_CFLAGS}
        ${WEBKITGTK_CFLAGS}
)
target_link_options(remidy-plugin-host PRIVATE
        ${GTK3_LDFLAGS}
        ${WEBKITGTK_LDFLAGS}
)

target_include_directories(remidy-plugin-host PRIVATE
        ../../external/choc
        ${cmidi2_SOURCE_DIR}
        ../../include
        remidy::remidy-webui
)
target_link_libraries(remidy-plugin-host PRIVATE
        saucer::saucer
        remidy::remidy
        remidy::remidy-tooling
        cpptrace::cpptrace
        uapmd
)
target_sources(remidy-plugin-host PRIVATE
        AppModel.cpp
        components/AudioDeviceSetup.cpp
        components/AudioPluginSelectors.cpp
        components/AudioPluginInstanceControl.cpp
        components/AudioPlayerController.cpp
        main.cpp
)

# Prepare web content files
add_custom_target(build-web-content
        npm install
        WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/web
)
add_dependencies(remidy-plugin-host build-web-content)

# depending on how heavyweight copying entire web directory (a bunch of node_modules) we may have to resort back to this.
if (0)
file(CREATE_LINK
        ${CMAKE_CURRENT_SOURCE_DIR}/web
        ${CMAKE_CURRENT_BINARY_DIR}/web
        COPY_ON_ERROR
        SYMBOLIC
)
else ()
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/web)
    file(COPY
            ${CMAKE_CURRENT_SOURCE_DIR}/web
            DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/
    )
endif ()
