cmake_minimum_required(VERSION 3.18)
project(uapmd-engine-tests)
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
add_executable(uapmd-project-file-tests
    UapmdProjectFileTest.cpp
    ../project/UapmdProjectFileReader.cpp
    ../project/UapmdProjectFileWriter.cpp
)

# Include directories
target_include_directories(uapmd-project-file-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_SOURCE_DIR}/include
    ${choc_SOURCE_DIR}
)

# Link libraries
target_link_libraries(uapmd-project-file-tests
    GTest::gtest_main
    GTest::gtest
)

# Discover tests
include(GoogleTest)
gtest_discover_tests(uapmd-project-file-tests)
