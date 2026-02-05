cmake_minimum_required(VERSION 3.18)
project(uapmd-data-tests)
set(CMAKE_CXX_STANDARD 23)

# Enable testing
enable_testing()

# Find or add GoogleTest
include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
)
# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# Create test executable
add_executable(uapmd-data-file-tests
    UapmdProjectFileTest.cpp
)

# Include directories
target_include_directories(uapmd-data-file-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_SOURCE_DIR}/include
    ${choc_SOURCE_DIR}
    ${midicci_SOURCE_DIR}/include
)

# Link libraries
target_link_libraries(uapmd-data-file-tests
    uapmd-data
    GTest::gtest_main
    GTest::gtest
)

# Discover tests
include(GoogleTest)
gtest_discover_tests(uapmd-data-file-tests)
