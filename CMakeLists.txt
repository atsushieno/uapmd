cmake_minimum_required(VERSION 3.18)

if (APPLE)
    project(UbiquitousAudioPluginMidiDevice LANGUAGES C CXX OBJC OBJCXX)
else()
    project(UbiquitousAudioPluginMidiDevice LANGUAGES C CXX)
endif()

set(CMAKE_CXX_STANDARD 23)

set(UAPMD_VERSION "0.1.2" CACHE STRING "Project version for packaging")

# wow, that's stupid... https://stackoverflow.com/questions/5004858/why-is-stdmin-failing-when-windows-h-is-included
if (MSVC OR WIN32)
add_compile_definitions(NOMINMAX)
endif()

include(cmake/CPM.cmake)
include(GNUInstallDirs)
set(CMAKE_INSTALL_DEFAULT_COMPONENT_NAME uapmd)


CPMAddPackage(
        NAME            choc
        GIT_TAG         68a63dd
        GIT_REPOSITORY  https://github.com/Tracktion/choc
        DOWNLOAD_ONLY YES
)

set(CMAKE_SKIP_INSTALL_RULES ON)
FetchContent_Declare(cmidi2
        GIT_REPOSITORY https://github.com/atsushieno/cmidi2
        GIT_TAG        0.9.3
        EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(cmidi2)

cmake_path(SET CPPLOCATE_PATCH_FILE NORMALIZE "${CMAKE_CURRENT_LIST_DIR}/external/cpplocate-remove-pthread.patch")
FetchContent_Declare(cpplocate
        NAME           cpplocate
        GIT_TAG        b796990d8df747758dbd6217b6315b7bdbb0c27e
        GIT_REPOSITORY https://github.com/cginternals/cpplocate
        # required to mix CPP and Swift in the same project...
        #PATCH_COMMAND        git apply "${CPPLOCATE_PATCH_FILE}"
        #UPDATE_DISCONNECTED 1 # https://stackoverflow.com/questions/73718516/
        EXCLUDE_FROM_ALL
)
# Force static build for cpplocate
set(_UAPMD_OLD_BUILD_SHARED_LIBS_CPPL "${BUILD_SHARED_LIBS}")
set(BUILD_SHARED_LIBS OFF)
FetchContent_MakeAvailable(cpplocate)
# Restore previous BUILD_SHARED_LIBS setting
if(DEFINED _UAPMD_OLD_BUILD_SHARED_LIBS_CPPL AND NOT "${_UAPMD_OLD_BUILD_SHARED_LIBS_CPPL}" STREQUAL "")
    set(BUILD_SHARED_LIBS "${_UAPMD_OLD_BUILD_SHARED_LIBS_CPPL}")
else()
    unset(BUILD_SHARED_LIBS)
endif()

FetchContent_Declare(midicci
        NAME            midicci
        GIT_TAG         8d68cb2
        GIT_REPOSITORY  https://github.com/atsushieno/midicci
        EXCLUDE_FROM_ALL
)
# Ensure we use a static build of midicci regardless of parent defaults
set(_UAPMD_OLD_BUILD_SHARED_LIBS "${BUILD_SHARED_LIBS}")
set(BUILD_SHARED_LIBS OFF)
FetchContent_MakeAvailable(midicci)
# Restore previous BUILD_SHARED_LIBS setting
if(DEFINED _UAPMD_OLD_BUILD_SHARED_LIBS AND NOT "${_UAPMD_OLD_BUILD_SHARED_LIBS}" STREQUAL "")
    set(BUILD_SHARED_LIBS "${_UAPMD_OLD_BUILD_SHARED_LIBS}")
else()
    unset(BUILD_SHARED_LIBS)
endif()
set(CMAKE_SKIP_INSTALL_RULES OFF)

CPMAddPackage(
        NAME portable-file-dialogs
        GIT_TAG c12ea8c
        GIT_REPOSITORY https://github.com/samhocevar/portable-file-dialogs
        DOWNLOAD_ONLY YES
)

CPMAddPackage(
        NAME           concurrentqueue
        GIT_TAG        2f09da73d22a47dc8a89cdd4fc4c3bfae07f4284
        GIT_REPOSITORY https://github.com/cameron314/concurrentqueue.git
        DOWNLOAD_ONLY YES
)


#add_compile_options("-fsanitize=address")
#link_libraries("-fsanitize=address")
#add_link_options(-fPIC)

add_subdirectory(src/remidy)
add_subdirectory(src/remidy-gui)
add_subdirectory(src/remidy-tooling)
add_subdirectory(src/uapmd)

add_subdirectory(tools)

# Provide an 'uninstall' target using the install manifest
if(NOT TARGET uninstall)
    configure_file(
            ${CMAKE_CURRENT_SOURCE_DIR}/cmake/cmake_uninstall.cmake.in
            ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake
            IMMEDIATE @ONLY)
    add_custom_target(uninstall
            COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake
            COMMENT "Uninstalling files from install manifest")
endif()

# Install public headers and in-tree libraries only
install(DIRECTORY include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        COMPONENT uapmd
        PATTERN "remidy-gui/outdated/*" EXCLUDE)

install(TARGETS
        remidy
        remidy-tooling
        remidy-gui
        uapmd
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        COMPONENT uapmd)

# -----------------
# CPack (Deb/RPM/TXZ)
# -----------------
set(CPACK_PACKAGE_NAME "uapmd")
set(CPACK_PACKAGE_VENDOR "uapmd")
set(CPACK_PACKAGE_CONTACT "uapmd maintainers")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Ubiquitous Audio Plugin MIDI Device tools and libraries")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/atsushieno/uapmd")

# Install under /usr in packages regardless of CMAKE_INSTALL_PREFIX on Linux
if(UNIX AND NOT APPLE)
    set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
endif()

# Generators per platform
if(WIN32)
    set(CPACK_GENERATOR "ZIP")
    find_program(UAPMD_MAKENSIS_EXECUTABLE NAMES makensis)
    if(UAPMD_MAKENSIS_EXECUTABLE)
        # Produce a Windows installer when NSIS is available
        set(CPACK_GENERATOR "${CPACK_GENERATOR};NSIS")
        set(CPACK_NSIS_DISPLAY_NAME "Ubiquitous Audio Plugin MIDI Device")
        set(CPACK_NSIS_PACKAGE_NAME "${CPACK_PACKAGE_NAME}")
        set(CPACK_NSIS_CONTACT "${CPACK_PACKAGE_CONTACT}")
        set(CPACK_NSIS_HELP_LINK "${CPACK_PACKAGE_HOMEPAGE_URL}")
        set(CPACK_NSIS_URL_INFO_ABOUT "${CPACK_PACKAGE_HOMEPAGE_URL}")
        set(CPACK_NSIS_MODIFY_PATH OFF)
    endif()
elseif(APPLE)
    set(CPACK_GENERATOR "DragNDrop")
    set(CPACK_DMG_VOLUME_NAME "UAPMD_${CPACK_PACKAGE_VERSION}")
    set(CPACK_DMG_SLA_USE_RESOURCE_FILE OFF)
    set(CPACK_DMG_DISABLE_APPLICATIONS_SYMLINK OFF)
else()
    # Linux still gets our usual native managers plus a tarball
    set(CPACK_GENERATOR "DEB;RPM;TXZ")
endif()

# Debian-specific
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${CPACK_PACKAGE_CONTACT}")
set(CPACK_DEBIAN_PACKAGE_SECTION "sound")
set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")

# RPM-specific
set(CPACK_RPM_PACKAGE_LICENSE "MIT")
set(CPACK_RPM_PACKAGE_GROUP "Applications/Multimedia")
set(CPACK_RPM_FILE_NAME "RPM-DEFAULT")

# Common metadata
set(CPACK_PACKAGE_VERSION "${UAPMD_VERSION}")

# Package components per-platform; DMG should only contain the app bundle
if(APPLE)
    set(CPACK_COMPONENTS_ALL uapmd_app)
    set(CPACK_COMPONENT_UAPMD_APP_DISPLAY_NAME "uapmd app bundle")
    set(CPACK_COMPONENT_UAPMD_APP_DESCRIPTION "UAPMD macOS desktop application bundle")
else()
    set(CPACK_COMPONENTS_ALL uapmd)
    set(CPACK_COMPONENT_UAPMD_DISPLAY_NAME "uapmd")
    set(CPACK_COMPONENT_UAPMD_DESCRIPTION "UAPMD tools, libraries, and headers")
endif()

include(CPack)

# -----------------
# Arch Linux helper
# -----------------
set(PACKAGING_ARCH_DIR "${CMAKE_BINARY_DIR}/packaging/arch")
file(MAKE_DIRECTORY "${PACKAGING_ARCH_DIR}")
configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/PKGBUILD.in
        ${PACKAGING_ARCH_DIR}/PKGBUILD
        @ONLY)
add_custom_target(package-arch-uapmd
        COMMAND ${CMAKE_COMMAND} -E echo "PKGBUILD generated at: ${PACKAGING_ARCH_DIR}/PKGBUILD"
        WORKING_DIRECTORY ${PACKAGING_ARCH_DIR}
        COMMENT "Generate Arch Linux PKGBUILD (use makepkg to build)")
