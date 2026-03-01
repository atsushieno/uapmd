cmake_minimum_required(VERSION 3.28)
project(UbiquitousAudioPluginMidiDevice LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 23)

#### Build options ####

option(UAPMD_ENABLE_WINMIDI "Enable Windows MIDI Services backend integration (Windows only)" ${WIN32})
option(UAPMD_CPPTRACE_DWARF_DEFAULT "Enable Windows MIDI Services backend integration (Windows only)" OFF)
option(UAPMD_TARGET_WASM "Build the WebAssembly variant of uapmd-app (requires emcmake/emscripten)" OFF)
option(UAPMD_ENABLE_ASAN "Enable AddressSanitizer (MSVC: /fsanitize=address, GCC/Clang: -fsanitize=address)" OFF)

#### end of section ####

set(UAPMD_VERSION "0.2.1" CACHE STRING "UAPMD version for packaging")

if (UNIX) # N/A on Windows
set(_UAPMD_CPPTRACE_DWARF_DEFAULT ${UAPMD_CPPTRACE_DWARF_DEFAULT})
endif()
if(ANDROID)
    set(_UAPMD_CPPTRACE_DWARF_DEFAULT OFF)
endif()
option(UAPMD_ENABLE_CPPTRACE_DWARF_SUPPORT "Enable cpptrace stack traces resolved via libdwarf" ${_UAPMD_CPPTRACE_DWARF_DEFAULT})
if(UAPMD_TARGET_WASM AND NOT EMSCRIPTEN)
    message(FATAL_ERROR "UAPMD_TARGET_WASM=ON requires configuring with the Emscripten toolchain (use emcmake cmake).")
endif()
if(EMSCRIPTEN)
    set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
endif()
set(UAPMD_WINMIDI_HEADERS_ARCHIVE "${CMAKE_CURRENT_LIST_DIR}/external/Microsoft.Windows.Devices.Midi2.1.0.14-rc.1.209.nupkg"
        CACHE FILEPATH "Path to the Windows MIDI Services header package used by libremidi")

# wow, that's stupid... https://stackoverflow.com/questions/5004858/why-is-stdmin-failing-when-windows-h-is-included
if (MSVC OR WIN32)
add_compile_definitions(NOMINMAX)
endif()

include(FetchContent)
include(cmake/CPM.cmake)
include(GNUInstallDirs)
set(CMAKE_INSTALL_DEFAULT_COMPONENT_NAME uapmd)

set(LIBREMIDI_HEADER_ONLY ON CACHE BOOL "Build libremidi as header-only" FORCE)
if(EMSCRIPTEN)
    add_compile_definitions(LIBLOCATE_DISABLE_DLOPEN)
    set(WITH_RUNTIME_CPU_DETECTION OFF CACHE BOOL "Disable zlib-ng SIMD dispatch for Emscripten" FORCE)
endif()

set(UAPMD_ENABLE_CPPTRACE ON)
if(EMSCRIPTEN)
    set(UAPMD_ENABLE_CPPTRACE OFF)
endif()
if(UAPMD_ENABLE_CPPTRACE)
    add_compile_definitions(UAPMD_HAS_CPPTRACE=1)
else()
    add_compile_definitions(UAPMD_HAS_CPPTRACE=0)
endif()
if(MSVC)
    # MSVC debug flags: /Zi for debug info, /Od for no optimization
    # Note: /Zi is required (not /ZI) when ASAN is enabled. /RTC1 must be absent (it conflicts with ASAN).
    set(CMAKE_CXX_FLAGS_DEBUG "/Zi /Od /MDd" CACHE STRING "" FORCE)
    set(CMAKE_C_FLAGS_DEBUG "/Zi /Od /MDd" CACHE STRING "" FORCE)
else()
    # we do not mess platforms that works totally well
endif()

if(UAPMD_ENABLE_ASAN)
    if(MSVC)
        add_compile_options(/fsanitize=address)
        # MSVC auto-links the ASAN runtime; no separate add_link_options needed.
        # The ASAN DLL (clang_rt.asan_dynamic-x86_64.dll / clang_rt.asan_dbg_dynamic-x86_64.dll)
        # must be on PATH or copied next to the executable at runtime.
        # Find it under: %VSINSTALLDIR%\VC\Tools\MSVC\<ver>\bin\Hostx64\x64\
    else()
        add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address)
    endif()
    message(STATUS "AddressSanitizer enabled")
endif()
set(CMAKE_COMPILE_WARNING_AS_ERROR OFF) # I don't approve GoogleTest warnaserror

# Block all dependency install rules globally
# This prevents third-party dependencies from polluting the installation
install(CODE "set(CMAKE_INSTALL_LOCAL_ONLY TRUE)" ALL_COMPONENTS)

if(UAPMD_ENABLE_CPPTRACE)
    set(_UAPMD_CPPTRACE_OPTIONS)
    if(UAPMD_ENABLE_CPPTRACE_DWARF_SUPPORT)
        message("WARNING: enabling DWARF support somehow results in bringing in extraneous installation files")
        list(APPEND _UAPMD_CPPTRACE_OPTIONS
                "CPPTRACE_GET_SYMBOLS_WITH_LIBDWARF ON"
                "CPPTRACE_DEMANGLE_WITH_CXXABI ON")
    endif()
    if(EMSCRIPTEN)
        list(APPEND _UAPMD_CPPTRACE_OPTIONS
                "CPPTRACE_UNWIND_WITH_NOTHING ON"
                "CPPTRACE_DISABLE_SIGNAL_HANDLERS ON")
    endif()
    CPMAddPackage(
            NAME           cpptrace
            GIT_TAG        v1.0.4
            GIT_REPOSITORY https://github.com/jeremy-rifkin/cpptrace.git
            EXCLUDE_FROM_ALL
            OPTIONS        ${_UAPMD_CPPTRACE_OPTIONS}
    )
    if(EMSCRIPTEN)
        set_property(TARGET cpptrace-lib PROPERTY CXX_SCAN_FOR_MODULES OFF)
    endif()

    # Force a static build of cpptrace regardless of parent defaults
    set(BUILD_SHARED_LIBS OFF)
endif()

CPMAddPackage(
        NAME           readerwriterqueue
        GIT_TAG        v1.0.7
        GIT_REPOSITORY https://github.com/cameron314/readerwriterqueue
        EXCLUDE_FROM_ALL
)

CPMAddPackage(
        NAME            choc
        GIT_TAG         1857a38
        GIT_REPOSITORY  https://github.com/Tracktion/choc
        DOWNLOAD_ONLY YES
)

set(UAPMD_ENABLE_CPPLOCATE ON)
if(ANDROID OR EMSCRIPTEN)
    set(UAPMD_ENABLE_CPPLOCATE OFF)
endif()
set(cpplocate_LIBRARIES "")

if (UAPMD_ENABLE_CPPLOCATE)
    CPMAddPackage(cpplocate
            NAME           cpplocate
            GIT_TAG        b796990d8df747758dbd6217b6315b7bdbb0c27e
            GIT_REPOSITORY https://github.com/cginternals/cpplocate
            EXCLUDE_FROM_ALL
    )
    set (cpplocate_LIBRARIES cpplocate::cpplocate)
endif ()

if(UAPMD_ENABLE_CPPLOCATE)
    # Force static build for cpplocate
    set(_UAPMD_OLD_BUILD_SHARED_LIBS_CPPL "${BUILD_SHARED_LIBS}")
    set(BUILD_SHARED_LIBS OFF)
    # Restore previous BUILD_SHARED_LIBS setting
    if(DEFINED _UAPMD_OLD_BUILD_SHARED_LIBS_CPPL AND NOT "${_UAPMD_OLD_BUILD_SHARED_LIBS_CPPL}" STREQUAL "")
        set(BUILD_SHARED_LIBS "${_UAPMD_OLD_BUILD_SHARED_LIBS_CPPL}")
    else()
        unset(BUILD_SHARED_LIBS)
    endif()
endif()

# Ensure we use a static build of midicci regardless of parent defaults
set(_UAPMD_OLD_BUILD_SHARED_LIBS "${BUILD_SHARED_LIBS}")
set(BUILD_SHARED_LIBS OFF)
CPMAddPackage(midicci
        NAME            midicci
        GIT_TAG         1e5a693
        GIT_REPOSITORY  https://github.com/atsushieno/midicci
        OPTIONS         "MIDICCI_SKIP_TOOLS ON"
        EXCLUDE_FROM_ALL
)
# Restore previous BUILD_SHARED_LIBS setting
if(DEFINED _UAPMD_OLD_BUILD_SHARED_LIBS AND NOT "${_UAPMD_OLD_BUILD_SHARED_LIBS}" STREQUAL "")
    set(BUILD_SHARED_LIBS "${_UAPMD_OLD_BUILD_SHARED_LIBS}")
else()
    unset(BUILD_SHARED_LIBS)
endif()
if(EMSCRIPTEN)
    if(TARGET umppi_static)
        set_target_properties(umppi_static PROPERTIES OUTPUT_NAME umppi_static)
    endif()
    if(TARGET midicci_static)
        set_target_properties(midicci_static PROPERTIES OUTPUT_NAME midicci_static)
    endif()
endif()

CPMAddPackage(
        NAME portable-file-dialogs
        GIT_TAG c12ea8c
        GIT_REPOSITORY https://github.com/samhocevar/portable-file-dialogs
        DOWNLOAD_ONLY YES
)

if(ANDROID)
    CPMAddPackage(
            NAME jmi
            GIT_TAG 7ef486e71ef2f34467775a5e3d05a7b5767c0547
            GIT_REPOSITORY https://github.com/wang-bin/JMI
            DOWNLOAD_ONLY YES
    )
endif()

CPMAddPackage(
        NAME           concurrentqueue
        GIT_TAG        2f09da73d22a47dc8a89cdd4fc4c3bfae07f4284
        GIT_REPOSITORY https://github.com/cameron314/concurrentqueue.git
        DOWNLOAD_ONLY YES
)

CPMAddPackage(
        NAME           miniaudio
        GIT_TAG        0.11.23
        GIT_REPOSITORY https://github.com/mackron/miniaudio
        DOWNLOAD_ONLY YES
)

# Re-enable installation for uapmd components
install(CODE "set(CMAKE_INSTALL_LOCAL_ONLY FALSE)" ALL_COMPONENTS)

#add_compile_options("-fsanitize=address")
#link_libraries("-fsanitize=address")
#add_link_options(-fPIC)

add_subdirectory(src/remidy)
add_subdirectory(src/remidy-gui)
add_subdirectory(src/remidy-tooling)
add_subdirectory(src/uapmd)
add_subdirectory(src/uapmd-file)
add_subdirectory(src/uapmd-data)
add_subdirectory(src/uapmd-engine)
if(EMSCRIPTEN AND TARGET liblocate)
    target_compile_definitions(liblocate PRIVATE LIBLOCATE_DISABLE_DLOPEN)
endif()

add_subdirectory(tools)

# Tests
if (NOT ANDROID)
option(UAPMD_BUILD_TESTS "Build uapmd tests" ON)
if(UAPMD_BUILD_TESTS)
    add_subdirectory(tests)
endif()
endif()


# INSTALL AND UNINSTALL targets
if (NOT ANDROID)

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
        uapmd-engine
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        COMPONENT uapmd)

# Install desktop file, icon, and AppStream metadata (Linux only)
if(UNIX AND NOT APPLE)
    install(FILES flatpak/dev.atsushieno.uapmd.desktop
            DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications
            COMPONENT uapmd)
    install(FILES flatpak/dev.atsushieno.uapmd.metainfo.xml
            DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/metainfo
            COMPONENT uapmd)
    install(FILES flatpak/dev.atsushieno.uapmd.svg
            DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps
            COMPONENT uapmd)
endif()

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
    # GitHub's macOS 15 runners can no longer build HFS+ DMGs reliably, so switch to APFS.
    set(CPACK_DMG_FILESYSTEM "APFS")
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

endif (NOT ANDROID)
