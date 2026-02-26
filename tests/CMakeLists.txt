cmake_minimum_required(VERSION 3.18)
project(uapmd-tests)
set(CMAKE_CXX_STANDARD 23)

# Enable testing
enable_testing()

# Find or add GoogleTest
# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
CPMAddPackage(
    NAME googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
)

# uapmd-data tests
add_executable(uapmd-project-file-tests
    UapmdProjectFileTest.cpp
)

target_include_directories(uapmd-project-file-tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${choc_SOURCE_DIR}
    ${midicci_SOURCE_DIR}/include
)

target_link_libraries(uapmd-project-file-tests
    uapmd-data
    GTest::gtest_main
    GTest::gtest
)

# Discover tests
include(GoogleTest)
gtest_discover_tests(uapmd-project-file-tests)
