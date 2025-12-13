cmake_minimum_required(VERSION 3.21)
project(remidy_c_api VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Find the remidy libraries
find_package(remidy REQUIRED HINTS ${CMAKE_SOURCE_DIR}/../cmake-build-debug)
find_package(remidy-tooling REQUIRED HINTS ${CMAKE_SOURCE_DIR}/../cmake-build-debug)

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
)

target_link_libraries(remidy_c
    PRIVATE
        remidy::remidy
        remidy-tooling::remidy-tooling
)

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
