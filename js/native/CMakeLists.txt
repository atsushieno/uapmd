cmake_minimum_required(VERSION 3.21)
project(remidy_c_api VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Find the remidy libraries
# Instead of using find_package, directly link to the built libraries
set(REMIDY_BUILD_DIR ${CMAKE_SOURCE_DIR}/../../cmake-build-debug)
set(REMIDY_LIB ${REMIDY_BUILD_DIR}/src/remidy/libremidy.a)
set(REMIDY_TOOLING_LIB ${REMIDY_BUILD_DIR}/src/remidy-tooling/libremidy-tooling.a)
set(REMIDY_GUI_LIB ${REMIDY_BUILD_DIR}/src/remidy-gui/libremidy-gui.a)

# Create shared library for FFI
add_library(remidy_c SHARED
    src/remidy_c.cpp
)

target_include_directories(remidy_c
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_SOURCE_DIR}/../include
        ${CMAKE_SOURCE_DIR}/../../include
        ${REMIDY_BUILD_DIR}/_deps/choc-src
        ${REMIDY_BUILD_DIR}/_deps/cmidi2-src/include
)

target_link_libraries(remidy_c
    PRIVATE
        ${REMIDY_LIB}
        ${REMIDY_TOOLING_LIB}
        ${REMIDY_GUI_LIB}
        ${REMIDY_BUILD_DIR}/lib/Debug/libsdk_hosting.a
        ${REMIDY_BUILD_DIR}/lib/Debug/libsdk_common.a
        ${REMIDY_BUILD_DIR}/lib/Debug/libsdk.a
        ${REMIDY_BUILD_DIR}/lib/Debug/libbase.a
        ${REMIDY_BUILD_DIR}/lib/Debug/libpluginterfaces.a
        ${REMIDY_BUILD_DIR}/_deps/cpplocate-build/libcpplocated.dylib
)

# Add platform-specific dependencies
if(APPLE)
    target_link_libraries(remidy_c PRIVATE
        "-framework CoreFoundation"
        "-framework CoreAudio"
        "-framework CoreMIDI"
        "-framework AudioToolbox"
        "-framework Foundation"
        "-framework AppKit"
        "-framework Cocoa"
        "-framework OpenGL"
    )
elseif(UNIX)
    find_package(X11 REQUIRED)
    target_link_libraries(remidy_c PRIVATE X11::X11)
endif()

# Find and link LV2 dependencies
find_library(LV2_LIB lv2)
find_library(LILV_LIB lilv-0)
find_library(SERD_LIB serd-0)
find_library(SORD_LIB sord-0)
find_library(SRATOM_LIB sratom-0)
find_library(ZIX_LIB zix-0)

if(LV2_LIB)
    target_link_libraries(remidy_c PRIVATE ${LV2_LIB})
endif()
if(LILV_LIB)
    target_link_libraries(remidy_c PRIVATE ${LILV_LIB})
endif()
if(SERD_LIB)
    target_link_libraries(remidy_c PRIVATE ${SERD_LIB})
endif()
if(SORD_LIB)
    target_link_libraries(remidy_c PRIVATE ${SORD_LIB})
endif()
if(SRATOM_LIB)
    target_link_libraries(remidy_c PRIVATE ${SRATOM_LIB})
endif()
if(ZIX_LIB)
    target_link_libraries(remidy_c PRIVATE ${ZIX_LIB})
endif()

# Set output directory
set_target_properties(remidy_c PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../dist/native
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../dist/native
)

# Install rules
install(TARGETS remidy_c
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

install(FILES include/remidy_c.h
    DESTINATION include
)
